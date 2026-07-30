// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2026 Axiado Corporation.
 *
 * Axiado AX3000 V4L2 capture driver.
 */

#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define AXIADO_VIDEO_DRIVER_NAME	"axiado-video"
#define AXIADO_VIDEO_CARD_NAME		"Axiado AX3000 Video"

#define AXIADO_VIDEO_MIN_WIDTH		320
#define AXIADO_VIDEO_MIN_HEIGHT		240
#define AXIADO_VIDEO_MAX_WIDTH		1920
#define AXIADO_VIDEO_MAX_HEIGHT		1080
#define AXIADO_VIDEO_DEF_WIDTH		640
#define AXIADO_VIDEO_DEF_HEIGHT		480
#define AXIADO_VIDEO_DEF_FRAMERATE	30
#define AXIADO_VIDEO_MIN_BUFFERS	2

/*
 * Host-written current mode (u32 LE width, u32 LE height) in the control
 * mailbox; must match AX_DP_MODE_DATA in the host's ax_drv.h.
 */
#define AX_DP_MODE_DATA		0x1000

struct axiado_video_buffer {
	struct vb2_v4l2_buffer	vb;
	struct list_head	link;
};

static inline struct axiado_video_buffer *
to_axiado_video_buffer(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct axiado_video_buffer, vb);
}

struct axiado_video {
	struct device		*dev;
	void			*addr;		/* memremap of shared region */
	size_t			addr_size;	/* bytes mapped */
	void __iomem		*ctrl_reg;	/* optional host control mailbox */

	struct v4l2_device	v4l2_dev;
	struct video_device	vdev;
	struct vb2_queue	queue;
	struct mutex		video_lock;	/* serialises queue + ioctls */

	spinlock_t		buf_lock;	/* protects pending list */
	struct list_head	pending;

	struct timer_list	timer;
	struct work_struct	capture_work;

	struct v4l2_pix_format	pix_fmt;
	unsigned int		frame_interval; /* jiffies between frames */
	unsigned int		fps;

	u32			sequence;
	bool			streaming;
};

struct axiado_video_format {
	u32	fourcc;
	u8	bpp;
};

static const struct axiado_video_format axiado_video_formats[] = {
	{ V4L2_PIX_FMT_XBGR32, 4 },
	{ V4L2_PIX_FMT_RGB24,  3 },
	{ V4L2_PIX_FMT_YUYV,   2 },
};

static const struct axiado_video_format *
axiado_video_lookup_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(axiado_video_formats); i++)
		if (axiado_video_formats[i].fourcc == fourcc)
			return &axiado_video_formats[i];
	return NULL;
}

static void axiado_video_apply_format(struct v4l2_pix_format *p,
				     const struct axiado_video_format *fmt)
{
	p->pixelformat = fmt->fourcc;
	p->bytesperline = p->width * fmt->bpp;
	p->sizeimage = p->bytesperline * p->height;
	p->field = V4L2_FIELD_NONE;
	p->colorspace = V4L2_COLORSPACE_SRGB;
}

static void axiado_video_clamp_size(struct v4l2_pix_format *p)
{
	p->width = clamp_t(u32, p->width & ~1u,
			   AXIADO_VIDEO_MIN_WIDTH, AXIADO_VIDEO_MAX_WIDTH);
	p->height = clamp_t(u32, p->height & ~1u,
			    AXIADO_VIDEO_MIN_HEIGHT, AXIADO_VIDEO_MAX_HEIGHT);
}

static void axiado_video_capture_one(struct axiado_video *video)
{
	struct axiado_video_buffer *buf;
	struct vb2_v4l2_buffer *vbuf;
	unsigned long flags;
	size_t copy_sz;
	void *vaddr;

	spin_lock_irqsave(&video->buf_lock, flags);
	if (!READ_ONCE(video->streaming) || list_empty(&video->pending)) {
		spin_unlock_irqrestore(&video->buf_lock, flags);
		return;
	}
	buf = list_first_entry(&video->pending, struct axiado_video_buffer, link);
	list_del_init(&buf->link);
	spin_unlock_irqrestore(&video->buf_lock, flags);

	vbuf = &buf->vb;
	vaddr = vb2_plane_vaddr(&vbuf->vb2_buf, 0);

	if (vaddr) {
		/*
		 * Prevent the CPU from reordering the loads below against
		 * prior loads.  This does not coordinate with the display
		 * engine writer; a torn frame is possible if the poll timer
		 * fires while the producer is mid-update.
		 */
		rmb();

		if (video->pix_fmt.pixelformat == V4L2_PIX_FMT_RGB24) {
			/*
			 * XBGR32 memory order: B(0), G(1), R(2), X(3).
			 * RGB24  memory order: R(0), G(1), B(2).
			 */
			const u8 *src = (const u8 *)video->addr;
			u8 *dst = (u8 *)vaddr;
			unsigned int pixels = video->pix_fmt.width * video->pix_fmt.height;
			unsigned int i;

			for (i = 0; i < pixels; i++) {
				dst[i * 3 + 0] = src[i * 4 + 2]; /* R */
				dst[i * 3 + 1] = src[i * 4 + 1]; /* G */
				dst[i * 3 + 2] = src[i * 4 + 0]; /* B */
			}

			copy_sz = pixels * 3;
			vb2_set_plane_payload(&vbuf->vb2_buf, 0, copy_sz);
		} else if (video->pix_fmt.pixelformat == V4L2_PIX_FMT_YUYV) {
			/*
			 * BT.601 studio-swing XBGR32→YUYV (4:2:2 packed).
			 * Source: B(0), G(1), R(2), X(3) per pixel.
			 * Output: Y0, U, Y1, V per pair of pixels.
			 * Width is always even (clamped in try_fmt).
			 */
			const u8 *src = (const u8 *)video->addr;
			u8 *dst = (u8 *)vaddr;
			unsigned int pairs = (video->pix_fmt.width * video->pix_fmt.height) / 2;
			unsigned int i;

			for (i = 0; i < pairs; i++) {
				s32 b0 = src[i * 8 + 0], g0 = src[i * 8 + 1], r0 = src[i * 8 + 2];
				s32 b1 = src[i * 8 + 4], g1 = src[i * 8 + 5], r1 = src[i * 8 + 6];

				dst[i * 4 + 0] = ((66 * r0 + 129 * g0 +  25 * b0 + 128) >> 8) + 16;
				dst[i * 4 + 1] = ((-38 * r0 - 74 * g0 + 112 * b0 + 128) >> 8) + 128;
				dst[i * 4 + 2] = ((66 * r1 + 129 * g1 +  25 * b1 + 128) >> 8) + 16;
				dst[i * 4 + 3] = ((112 * r0 - 94 * g0 - 18 * b0 + 128) >> 8) + 128;
			}

			copy_sz = video->pix_fmt.width * video->pix_fmt.height * 2;
			vb2_set_plane_payload(&vbuf->vb2_buf, 0, copy_sz);
		} else {
			copy_sz = min_t(size_t, video->pix_fmt.sizeimage, video->addr_size);
			memcpy(vaddr, video->addr, copy_sz);
			vb2_set_plane_payload(&vbuf->vb2_buf, 0, copy_sz);
		}
	}

	vbuf->vb2_buf.timestamp = ktime_get_ns();
	vbuf->sequence = video->sequence++;
	vbuf->field = V4L2_FIELD_NONE;
	vb2_buffer_done(&vbuf->vb2_buf, vaddr ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
}

static void axiado_video_capture_work(struct work_struct *work)
{
	struct axiado_video *video = container_of(work, struct axiado_video, capture_work);

	axiado_video_capture_one(video);

	if (READ_ONCE(video->streaming))
		mod_timer(&video->timer, jiffies + READ_ONCE(video->frame_interval));
}

static void axiado_video_timer_fn(struct timer_list *t)
{
	struct axiado_video *video = timer_container_of(video, t, timer);

	schedule_work(&video->capture_work);
}

static int axiado_video_queue_setup(struct vb2_queue *q,
				   unsigned int *nbuffers,
				   unsigned int *nplanes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct axiado_video *video = vb2_get_drv_priv(q);
	unsigned int size = video->pix_fmt.sizeimage;

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
		return 0;
	}
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int axiado_video_buf_prepare(struct vb2_buffer *vb)
{
	struct axiado_video *video = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long sz = video->pix_fmt.sizeimage;

	if (vb2_plane_size(vb, 0) < sz) {
		dev_err(video->dev,
			"buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), sz);
		return -EINVAL;
	}
	return 0;
}

static void axiado_video_buf_queue(struct vb2_buffer *vb)
{
	struct axiado_video *video = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct axiado_video_buffer *buf = to_axiado_video_buffer(vbuf);
	unsigned long flags;

	spin_lock_irqsave(&video->buf_lock, flags);
	list_add_tail(&buf->link, &video->pending);
	spin_unlock_irqrestore(&video->buf_lock, flags);
}

static int axiado_video_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct axiado_video *video = vb2_get_drv_priv(q);

	video->sequence = 0;
	WRITE_ONCE(video->streaming, true);
	mod_timer(&video->timer, jiffies + READ_ONCE(video->frame_interval));

	return 0;
}

static void axiado_video_stop_streaming(struct vb2_queue *q)
{
	struct axiado_video *video = vb2_get_drv_priv(q);
	struct axiado_video_buffer *buf, *tmp;
	struct list_head drain;
	unsigned long flags;

	WRITE_ONCE(video->streaming, false);
	timer_delete_sync(&video->timer);
	cancel_work_sync(&video->capture_work);
	/*
	 * A capture_work instance that read streaming==true before the
	 * WRITE_ONCE above may have called mod_timer() just before
	 * cancel_work_sync() returned.  Delete again to close that window.
	 */
	timer_delete_sync(&video->timer);

	INIT_LIST_HEAD(&drain);
	spin_lock_irqsave(&video->buf_lock, flags);
	list_splice_init(&video->pending, &drain);
	spin_unlock_irqrestore(&video->buf_lock, flags);

	list_for_each_entry_safe(buf, tmp, &drain, link) {
		list_del_init(&buf->link);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops axiado_video_vb2_ops = {
	.queue_setup		= axiado_video_queue_setup,
	.buf_prepare		= axiado_video_buf_prepare,
	.buf_queue		= axiado_video_buf_queue,
	.start_streaming	= axiado_video_start_streaming,
	.stop_streaming		= axiado_video_stop_streaming,
};

static int axiado_video_querycap(struct file *file, void *fh,
				struct v4l2_capability *cap)
{
	struct axiado_video *video = video_drvdata(file);

	strscpy(cap->driver, AXIADO_VIDEO_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, AXIADO_VIDEO_CARD_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(video->dev));
	return 0;
}

static int axiado_video_enum_fmt(struct file *file, void *fh,
				struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(axiado_video_formats))
		return -EINVAL;
	f->pixelformat = axiado_video_formats[f->index].fourcc;
	return 0;
}

static int axiado_video_g_fmt(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct axiado_video *video = video_drvdata(file);

	f->fmt.pix = video->pix_fmt;
	return 0;
}

/*
 * Reject formats whose sizeimage, or whose 4-bpp XBGR32 source read for
 * RGB24/YUYV, would not fit in the mapped shared region.  Shared by
 * TRY_FMT, S_FMT and S_DV_TIMINGS so all three ioctls agree on which
 * formats are actually usable.
 */
static int axiado_video_check_addr_size(const struct axiado_video *video,
					const struct v4l2_pix_format *p)
{
	if (p->sizeimage > video->addr_size) {
		dev_dbg(video->dev,
			"sizeimage %u exceeds shared region %zu\n",
			p->sizeimage, video->addr_size);
		return -EINVAL;
	}

	if ((p->pixelformat == V4L2_PIX_FMT_RGB24 ||
	     p->pixelformat == V4L2_PIX_FMT_YUYV) &&
	    (size_t)p->width * p->height * 4 > video->addr_size) {
		dev_dbg(video->dev,
			"source size %zu exceeds shared region %zu\n",
			(size_t)p->width * p->height * 4, video->addr_size);
		return -EINVAL;
	}

	return 0;
}

static int axiado_video_try_fmt(struct file *file, void *fh,
			       struct v4l2_format *f)
{
	struct axiado_video *video = video_drvdata(file);
	const struct axiado_video_format *fmt;

	fmt = axiado_video_lookup_format(f->fmt.pix.pixelformat);
	if (!fmt)
		fmt = &axiado_video_formats[0];

	axiado_video_clamp_size(&f->fmt.pix);
	axiado_video_apply_format(&f->fmt.pix, fmt);

	return axiado_video_check_addr_size(video, &f->fmt.pix);
}

static int axiado_video_s_fmt(struct file *file, void *fh,
			     struct v4l2_format *f)
{
	struct axiado_video *video = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&video->queue))
		return -EBUSY;

	ret = axiado_video_try_fmt(file, fh, f);
	if (ret)
		return ret;

	video->pix_fmt = f->fmt.pix;
	return 0;
}

static int axiado_video_enum_framesizes(struct file *file, void *fh,
				       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index)
		return -EINVAL;
	if (!axiado_video_lookup_format(fsize->pixel_format))
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width  = AXIADO_VIDEO_MIN_WIDTH;
	fsize->stepwise.min_height = AXIADO_VIDEO_MIN_HEIGHT;
	fsize->stepwise.max_width  = AXIADO_VIDEO_MAX_WIDTH;
	fsize->stepwise.max_height = AXIADO_VIDEO_MAX_HEIGHT;
	fsize->stepwise.step_width  = 2;
	fsize->stepwise.step_height = 2;
	return 0;
}

static int axiado_video_enum_input(struct file *file, void *fh,
				  struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;
	strscpy(inp->name, "Camera", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	return 0;
}

static int axiado_video_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int axiado_video_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static int axiado_video_g_parm(struct file *file, void *fh,
			      struct v4l2_streamparm *parm)
{
	struct axiado_video *video = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = READ_ONCE(video->fps);
	parm->parm.capture.readbuffers = AXIADO_VIDEO_MIN_BUFFERS;
	return 0;
}

static int axiado_video_s_parm(struct file *file, void *fh,
			      struct v4l2_streamparm *parm)
{
	struct axiado_video *video = video_drvdata(file);
	unsigned int num, denom, fps;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_is_busy(&video->queue))
		return -EBUSY;

	num = parm->parm.capture.timeperframe.numerator;
	denom = parm->parm.capture.timeperframe.denominator;
	if (!num || !denom)
		return -EINVAL;
	fps = clamp(denom / num, 1u, 120u);

	WRITE_ONCE(video->fps, fps);
	WRITE_ONCE(video->frame_interval, max(1u, HZ / fps));

	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe.numerator = 1;
	parm->parm.capture.timeperframe.denominator = fps;
	parm->parm.capture.readbuffers = AXIADO_VIDEO_MIN_BUFFERS;
	return 0;
}

/*
 * No digital-video receiver is present; synthesise BT.656/1120 timings
 * from pix_fmt so that clients using VIDIOC_G/S_DV_TIMINGS to set
 * the capture geometry see consistent width and height across all ioctls.
 */
static void axiado_video_fill_timings(const struct axiado_video *video,
				      struct v4l2_dv_timings *timings)
{
	memset(timings, 0, sizeof(*timings));
	timings->type = V4L2_DV_BT_656_1120;
	timings->bt.width = video->pix_fmt.width;
	timings->bt.height = video->pix_fmt.height;
	timings->bt.interlaced = V4L2_DV_PROGRESSIVE;
}

/*
 * Read the mode the host is actually driving from the control mailbox,
 * falling back to the configured capture format if it isn't mapped or
 * hasn't been written yet.
 */
static void axiado_video_detect_timings(const struct axiado_video *video,
					struct v4l2_dv_timings *timings)
{
	u32 width, height;

	if (video->ctrl_reg) {
		width = ioread32(video->ctrl_reg + AX_DP_MODE_DATA);
		height = ioread32(video->ctrl_reg + AX_DP_MODE_DATA + 4);

		if (width >= AXIADO_VIDEO_MIN_WIDTH &&
		    width <= AXIADO_VIDEO_MAX_WIDTH &&
		    height >= AXIADO_VIDEO_MIN_HEIGHT &&
		    height <= AXIADO_VIDEO_MAX_HEIGHT) {
			memset(timings, 0, sizeof(*timings));
			timings->type = V4L2_DV_BT_656_1120;
			timings->bt.width = width & ~1u;
			timings->bt.height = height & ~1u;
			timings->bt.interlaced = V4L2_DV_PROGRESSIVE;
			return;
		}
	}

	axiado_video_fill_timings(video, timings);
}

static int axiado_video_s_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct axiado_video *video = video_drvdata(file);
	const struct axiado_video_format *fmt;
	struct v4l2_pix_format pix;
	int ret;

	if (timings->type != V4L2_DV_BT_656_1120)
		return -EINVAL;

	if (vb2_is_busy(&video->queue))
		return -EBUSY;

	pix = video->pix_fmt;
	pix.width  = timings->bt.width;
	pix.height = timings->bt.height;
	axiado_video_clamp_size(&pix);

	fmt = axiado_video_lookup_format(pix.pixelformat);
	if (!fmt)
		fmt = &axiado_video_formats[0];
	axiado_video_apply_format(&pix, fmt);

	ret = axiado_video_check_addr_size(video, &pix);
	if (ret)
		return ret;

	video->pix_fmt = pix;
	axiado_video_fill_timings(video, timings);
	return 0;
}

static int axiado_video_g_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	axiado_video_fill_timings(video_drvdata(file), timings);
	return 0;
}

static int axiado_video_query_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	axiado_video_detect_timings(video_drvdata(file), timings);
	return 0;
}

static int axiado_video_enum_dv_timings(struct file *file, void *fh,
		struct v4l2_enum_dv_timings *timings)
{
	/* No predefined timings; resolution is set freely via S_DV_TIMINGS. */
	return -EINVAL;
}

static int axiado_video_dv_timings_cap(struct file *file, void *fh,
		struct v4l2_dv_timings_cap *cap)
{
	cap->type = V4L2_DV_BT_656_1120;
	cap->bt.min_width  = AXIADO_VIDEO_MIN_WIDTH;
	cap->bt.max_width  = AXIADO_VIDEO_MAX_WIDTH;
	cap->bt.min_height = AXIADO_VIDEO_MIN_HEIGHT;
	cap->bt.max_height = AXIADO_VIDEO_MAX_HEIGHT;
	cap->bt.standards  = 0;
	cap->bt.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE;
	return 0;
}

static const struct v4l2_ioctl_ops axiado_video_ioctl_ops = {
	.vidioc_querycap		= axiado_video_querycap,
	.vidioc_enum_fmt_vid_cap	= axiado_video_enum_fmt,
	.vidioc_g_fmt_vid_cap		= axiado_video_g_fmt,
	.vidioc_s_fmt_vid_cap		= axiado_video_s_fmt,
	.vidioc_try_fmt_vid_cap		= axiado_video_try_fmt,
	.vidioc_enum_framesizes		= axiado_video_enum_framesizes,
	.vidioc_enum_input		= axiado_video_enum_input,
	.vidioc_g_input			= axiado_video_g_input,
	.vidioc_s_input			= axiado_video_s_input,
	.vidioc_g_parm			= axiado_video_g_parm,
	.vidioc_s_parm			= axiado_video_s_parm,
	.vidioc_s_dv_timings		= axiado_video_s_dv_timings,
	.vidioc_g_dv_timings		= axiado_video_g_dv_timings,
	.vidioc_query_dv_timings	= axiado_video_query_dv_timings,
	.vidioc_enum_dv_timings		= axiado_video_enum_dv_timings,
	.vidioc_dv_timings_cap		= axiado_video_dv_timings_cap,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations axiado_video_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

static int axiado_video_init_queue(struct axiado_video *video)
{
	struct vb2_queue *q = &video->queue;

	q->type		= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes	= VB2_MMAP | VB2_READ;
	q->drv_priv	= video;
	q->buf_struct_size = sizeof(struct axiado_video_buffer);
	q->ops		= &axiado_video_vb2_ops;
	q->mem_ops	= &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = AXIADO_VIDEO_MIN_BUFFERS;
	q->lock		= &video->video_lock;

	return vb2_queue_init(q);
}

/*
 * Called when the last reference on v4l2_dev drops, i.e. after both
 * vb2_video_unregister_device() has run in remove() AND all open file
 * descriptors have been released.  This is the documented V4L2 lifetime
 * mechanism for an embedded struct video_device (v4l2-dev.rst).
 */
static void axiado_v4l2_device_release(struct v4l2_device *v4l2_dev)
{
	struct axiado_video *video =
		container_of(v4l2_dev, struct axiado_video, v4l2_dev);

	memunmap(video->addr);
	kfree(video);
}

static int axiado_video_probe(struct platform_device *pdev)
{
	struct axiado_video *video;
	struct device_node *node;
	struct resource res;
	int ret;

	video = kzalloc_obj(*video, GFP_KERNEL);
	if (!video)
		return -ENOMEM;

	video->dev = &pdev->dev;
	platform_set_drvdata(pdev, video);

	mutex_init(&video->video_lock);
	spin_lock_init(&video->buf_lock);
	INIT_LIST_HEAD(&video->pending);
	timer_setup(&video->timer, axiado_video_timer_fn, 0);
	INIT_WORK(&video->capture_work, axiado_video_capture_work);

	video->pix_fmt.width  = AXIADO_VIDEO_DEF_WIDTH;
	video->pix_fmt.height = AXIADO_VIDEO_DEF_HEIGHT;
	axiado_video_apply_format(&video->pix_fmt, &axiado_video_formats[0]);
	video->fps = AXIADO_VIDEO_DEF_FRAMERATE;
	video->frame_interval = max(1u, HZ / AXIADO_VIDEO_DEF_FRAMERATE);

	node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!node) {
		ret = dev_err_probe(&pdev->dev, -EINVAL, "missing memory-region\n");
		goto err_free;
	}
	ret = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (ret) {
		ret = dev_err_probe(&pdev->dev, ret, "memory-region address invalid\n");
		goto err_free;
	}

	video->addr_size = resource_size(&res);
	video->addr = memremap(res.start, video->addr_size, MEMREMAP_WC);
	if (!video->addr) {
		ret = dev_err_probe(&pdev->dev, -ENOMEM,
				    "memremap of shared region failed\n");
		goto err_free;
	}

	if (video->pix_fmt.sizeimage > video->addr_size) {
		ret = dev_err_probe(&pdev->dev, -EINVAL,
				    "shared region %zu too small for default %ux%u XBGR32 frame\n",
				    video->addr_size,
				    AXIADO_VIDEO_DEF_WIDTH, AXIADO_VIDEO_DEF_HEIGHT);
		goto err_memunmap;
	}

	/* Optional control mailbox (second memory-region entry). */
	node = of_parse_phandle(pdev->dev.of_node, "memory-region", 1);
	if (node) {
		struct resource ctrl_res;

		ret = of_address_to_resource(node, 0, &ctrl_res);
		of_node_put(node);
		if (!ret) {
			video->ctrl_reg = devm_memremap(&pdev->dev, ctrl_res.start,
							resource_size(&ctrl_res),
							MEMREMAP_WB);
			if (!video->ctrl_reg)
				dev_warn(&pdev->dev,
					 "failed to map control mailbox; DV timings will echo capture format\n");
		}
	}

	ret = v4l2_device_register(&pdev->dev, &video->v4l2_dev);
	if (ret)
		goto err_memunmap;
	video->v4l2_dev.release = axiado_v4l2_device_release;

	ret = axiado_video_init_queue(video);
	if (ret)
		goto err_v4l2_unreg;

	video->vdev.fops	= &axiado_video_fops;
	video->vdev.ioctl_ops	= &axiado_video_ioctl_ops;
	video->vdev.v4l2_dev	= &video->v4l2_dev;
	video->vdev.queue	= &video->queue;
	video->vdev.lock	= &video->video_lock;
	video->vdev.release	= video_device_release_empty;
	video->vdev.vfl_dir	= VFL_DIR_RX;
	video->vdev.device_caps	= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				  V4L2_CAP_READWRITE;
	strscpy(video->vdev.name, AXIADO_VIDEO_DRIVER_NAME,
		sizeof(video->vdev.name));
	video_set_drvdata(&video->vdev, video);

	ret = video_register_device(&video->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&pdev->dev, "video_register_device: %d\n", ret);
		goto err_v4l2_unreg;
	}

	dev_info(&pdev->dev, "registered as /dev/video%d\n",
		 video->vdev.num);
	return 0;

err_v4l2_unreg:
	v4l2_device_put(&video->v4l2_dev);
	return ret;
err_memunmap:
	memunmap(video->addr);
err_free:
	kfree(video);
	return ret;
}

static void axiado_video_remove(struct platform_device *pdev)
{
	struct axiado_video *video = platform_get_drvdata(pdev);

	vb2_video_unregister_device(&video->vdev);
	/*
	 * Drop the reference held since v4l2_device_register().  If all file
	 * descriptors are already closed this triggers axiado_v4l2_device_release
	 * immediately; otherwise it fires when the last fd is released.
	 */
	v4l2_device_put(&video->v4l2_dev);
}

static int axiado_video_suspend(struct device *dev)
{
	struct axiado_video *video = dev_get_drvdata(dev);

	if (!READ_ONCE(video->streaming))
		return 0;

	/*
	 * Clear streaming before stopping so that any capture_work instance
	 * still executing won't re-arm the timer.  Apply the same
	 * double-delete pattern used in stop_streaming to close the race
	 * window between the work's READ_ONCE and the timer re-arm.
	 */
	WRITE_ONCE(video->streaming, false);
	timer_delete_sync(&video->timer);
	cancel_work_sync(&video->capture_work);
	timer_delete_sync(&video->timer);
	return 0;
}

static int axiado_video_resume(struct device *dev)
{
	struct axiado_video *video = dev_get_drvdata(dev);

	/*
	 * vb2_is_streaming() is the authoritative source for whether capture
	 * was active before suspend, since we cleared video->streaming above.
	 */
	if (vb2_is_streaming(&video->queue)) {
		WRITE_ONCE(video->streaming, true);
		mod_timer(&video->timer, jiffies + READ_ONCE(video->frame_interval));
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(axiado_video_pm_ops,
				axiado_video_suspend, axiado_video_resume);

static const struct of_device_id axiado_video_of_match[] = {
	{ .compatible = "axiado,ax3000-video" },
	{ }
};
MODULE_DEVICE_TABLE(of, axiado_video_of_match);

static struct platform_driver axiado_video_driver = {
	.driver = {
		.name		= AXIADO_VIDEO_DRIVER_NAME,
		.of_match_table	= axiado_video_of_match,
		.pm		= pm_sleep_ptr(&axiado_video_pm_ops),
	},
	.probe	= axiado_video_probe,
	.remove	= axiado_video_remove,
};
module_platform_driver(axiado_video_driver);

MODULE_AUTHOR("Axiado Corporation");
MODULE_DESCRIPTION("Axiado AX3000 V4L2 video capture driver");
MODULE_LICENSE("GPL");
