// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H Input Video Control Block driver
 *
 * Copyright (C) 2024 Ideas on Board Oy
 */

#include <linux/cleanup.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/media-bus-format.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>
#include <linux/wait.h>

#include <media/media-jobs.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "rzv2h-ivc.h"

#define RZV2H_IVC_FIXED_HBLANK			0x20
#define RZV2H_IVC_MIN_VBLANK			27

#define to_rzv2h_ivc_buf(vbuf) \
	container_of(vbuf, struct rzv2h_ivc_buf, vb)

static const struct rzv2h_ivc_format rzv2h_ivc_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_SBGGR8,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR8_1X8,
		},
		.dtype = MIPI_CSI2_DT_RAW8,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG8,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGBRG8_1X8,
		},
		.dtype = MIPI_CSI2_DT_RAW8,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG8,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGRBG8_1X8,
		},
		.dtype = MIPI_CSI2_DT_RAW8,
	},
	{
		.fourcc = V4L2_PIX_FMT_SRGGB8,
		.mbus_codes = {
			MEDIA_BUS_FMT_SRGGB8_1X8,
		},
		.dtype = MIPI_CSI2_DT_RAW8,
	},
	{
		.fourcc = V4L2_PIX_FMT_CRU_RAW10,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR10_1X10,
			MEDIA_BUS_FMT_SGBRG10_1X10,
			MEDIA_BUS_FMT_SGRBG10_1X10,
			MEDIA_BUS_FMT_SRGGB10_1X10
		},
		.dtype = MIPI_CSI2_DT_RAW10,
	},
	{
		.fourcc = V4L2_PIX_FMT_CRU_RAW12,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR12_1X12,
			MEDIA_BUS_FMT_SGBRG12_1X12,
			MEDIA_BUS_FMT_SGRBG12_1X12,
			MEDIA_BUS_FMT_SRGGB12_1X12
		},
		.dtype = MIPI_CSI2_DT_RAW12,
	},
	{
		.fourcc = V4L2_PIX_FMT_CRU_RAW14,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR14_1X14,
			MEDIA_BUS_FMT_SGBRG14_1X14,
			MEDIA_BUS_FMT_SGRBG14_1X14,
			MEDIA_BUS_FMT_SRGGB14_1X14
		},
		.dtype = MIPI_CSI2_DT_RAW14,
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR16,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR16_1X16,
		},
		.dtype = MIPI_CSI2_DT_RAW16,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG16,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGBRG16_1X16,
		},
		.dtype = MIPI_CSI2_DT_RAW16,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG16,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGRBG16_1X16,
		},
		.dtype = MIPI_CSI2_DT_RAW16,
	},
	{
		.fourcc = V4L2_PIX_FMT_SRGGB16,
		.mbus_codes = {
			MEDIA_BUS_FMT_SRGGB16_1X16,
		},
		.dtype = MIPI_CSI2_DT_RAW16,
	},
};

static int rzv2h_ivc_pipeline_started(struct media_entity *entity)
{
	struct video_device *vdev = media_entity_to_video_device(entity);
	struct rzv2h_ivc *ivc = video_get_drvdata(vdev);

	/*
	 * With min_queued_buffers set to 1, we know that we must have at least
	 * a single buffer to start feeding, so we can fetch that now and fire
	 * it off to the ISP.
	 */
	ivc->buffers.sequence = 0;
	rzv2h_ivc_send_next_buffer(ivc);

	return 0;
}

static void rzv2h_ivc_pipeline_stopped(struct media_entity *entity)
{
	struct video_device *vdev = media_entity_to_video_device(entity);
	struct rzv2h_ivc *ivc = video_get_drvdata(vdev);
	u32 val = 0;

	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_FM_STOP, 0x1);
	readl_poll_timeout(ivc->base + RZV2H_IVC_REG_FM_STOP,
			   val, !val, 10 * USEC_PER_MSEC, 250 * USEC_PER_MSEC);
}

static const struct media_entity_operations rzv2h_ivc_media_ops = {
	.pipeline_started = rzv2h_ivc_pipeline_started,
	.pipeline_stopped = rzv2h_ivc_pipeline_stopped,
};

static bool rzv2h_ivc_job_check_dep(void *data)
{
	struct rzv2h_ivc *ivc = data;

	guard(spinlock)(&ivc->buffers.lock);

	if (list_empty(&ivc->buffers.pending))
		return false;

	return true;
}

static void rzv2h_ivc_job_clear_dep(void *data)
{
	struct rzv2h_ivc *ivc = data;
	struct rzv2h_ivc_buf *buf;

	/*
	 * We need to move an entry from the pending queue to the input queue
	 * here. We know that there is one, or .check_dep() would not have
	 * allowed us to get this far. The entry needs to be removed or the same
	 * check would allow a new job to be queued for the exact same buffer.
	 */
	guard(spinlock)(&ivc->buffers.lock);
	buf = list_first_entry(&ivc->buffers.pending,
			       struct rzv2h_ivc_buf, queue);
	list_move_tail(&buf->queue, &ivc->buffers.queue);
}

static void rzv2h_ivc_job_reset_dep(void *data)
{
	struct rzv2h_ivc *ivc = data;
	struct rzv2h_ivc_buf *buf;

	guard(spinlock)(&ivc->buffers.lock);
	buf = list_first_entry(&ivc->buffers.queue,
			       struct rzv2h_ivc_buf, queue);

	if (buf)
		list_move(&buf->queue, &ivc->buffers.pending);
}

static void rzv2h_ivc_job_set_next_buffer(void *data)
{
	struct rzv2h_ivc *ivc = data;

	rzv2h_ivc_set_next_buffer(ivc);
}

static void rzv2h_ivc_job_send_next_buffer(void *data)
{
	struct rzv2h_ivc *ivc = data;

	rzv2h_ivc_send_next_buffer(ivc);
}

static struct media_job_dep_ops rzv2h_ivc_media_job_dep_ops = {
	.check_dep = rzv2h_ivc_job_check_dep,
	.clear_dep = rzv2h_ivc_job_clear_dep,
	.reset_dep = rzv2h_ivc_job_reset_dep,
};

static int rzv2h_ivc_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				 unsigned int *num_planes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct rzv2h_ivc *ivc = vb2_get_drv_priv(q);

	if (*num_planes && *num_planes > 1)
		return -EINVAL;

	if (sizes[0] && sizes[0] < ivc->format.pix.sizeimage)
		return -EINVAL;

	*num_planes = 1;

	if (!sizes[0])
		sizes[0] = ivc->format.pix.sizeimage;

	return 0;
}

static void rzv2h_ivc_buf_queue(struct vb2_buffer *vb)
{
	struct rzv2h_ivc *ivc = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rzv2h_ivc_buf *buf = to_rzv2h_ivc_buf(vbuf);

	spin_lock(&ivc->buffers.lock);
	list_add_tail(&buf->queue, &ivc->buffers.pending);
	spin_unlock(&ivc->buffers.lock);

	media_jobs_try_queue_job(ivc->sched, MEDIA_JOB_TYPE_PIPELINE_PULSE,
				 &rzv2h_ivc_media_job_dep_ops, ivc);
}

static int rzv2h_ivc_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rzv2h_ivc_buf *buf = to_rzv2h_ivc_buf(vbuf);

	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);

	return 0;
}

static void rzv2h_ivc_format_configure(struct rzv2h_ivc *ivc)
{
	const struct rzv2h_ivc_format *fmt = ivc->format.fmt;
	struct v4l2_pix_format *pix = &ivc->format.pix;
	unsigned int min_vblank;
	unsigned int vblank;
	unsigned int hts;

	/* Currently only CRU packed pixel formats are supported */
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_PXFMT,
			RZV2H_IVC_INPUT_FMT_CRU_PACKED);

	rzv2h_ivc_update_bits(ivc, RZV2H_IVC_REG_AXIRX_PXFMT,
			      RZV2H_IVC_PXFMT_DTYPE, fmt->dtype);

	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_HSIZE, pix->width);
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_VSIZE, pix->height);
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_STRD, pix->bytesperline);

	/*
	 * The ISP has minimum vertical blanking requirements that must be
	 * adhered to by the IVC. The minimum is a function of the Iridix blocks
	 * clocking requirements and the width of the image and horizontal
	 * blanking, but if we assume the worst case then it boils down to the
	 * below (plus one to the numerator to ensure the answer is rounded up)
	 */

	hts = pix->width + RZV2H_IVC_FIXED_HBLANK;
	min_vblank = 15 + (120501 / hts);
	vblank = max(min_vblank, RZV2H_IVC_MIN_VBLANK);

	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_BLANK,
			RZV2H_IVC_VBLANK(vblank));
}

void rzv2h_ivc_set_next_buffer(struct rzv2h_ivc *ivc)
{
	struct rzv2h_ivc_buf *buf;

	guard(spinlock)(&ivc->buffers.lock);

	if (ivc->buffers.curr) {
		ivc->buffers.curr->vb.sequence = ivc->buffers.sequence++;
		vb2_buffer_done(&ivc->buffers.curr->vb.vb2_buf,
				VB2_BUF_STATE_DONE);
		ivc->buffers.curr = NULL;
	}

	buf = list_first_entry_or_null(&ivc->buffers.queue,
				       struct rzv2h_ivc_buf, queue);
	if (buf)
		list_del(&buf->queue);
	else
		return;

	ivc->buffers.curr = buf;
	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_AXIRX_SADDL_P0, buf->addr);
}

void rzv2h_ivc_send_next_buffer(struct rzv2h_ivc *ivc)
{
	wait_event_interruptible_timeout(ivc->buffers.wq, !ivc->vvalid_ifp,
					 msecs_to_jiffies(20));

	spin_lock(&ivc->spinlock);
	ivc->vvalid_ifp = 2;
	spin_unlock(&ivc->spinlock);

	rzv2h_ivc_write(ivc, RZV2H_IVC_REG_FM_FRCON, 0x1);
}

static void rzv2h_ivc_return_buffers(struct rzv2h_ivc *ivc,
				     enum vb2_buffer_state state)
{
	struct rzv2h_ivc_buf *buf, *tmp;

	guard(spinlock)(&ivc->buffers.lock);

	if (ivc->buffers.curr) {
		vb2_buffer_done(&ivc->buffers.curr->vb.vb2_buf, state);
		ivc->buffers.curr = NULL;
	}

	list_for_each_entry_safe(buf, tmp, &ivc->buffers.pending, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	list_for_each_entry_safe(buf, tmp, &ivc->buffers.queue, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static bool rzv2h_ivc_pipeline_ready(struct media_pipeline *pipe)
{
	struct media_pipeline_entity_iter iter;
	unsigned int n_video_devices = 0;
	struct media_entity *entity;
	int ret;

	ret = media_pipeline_entity_iter_init(pipe, &iter);
	if (ret)
		return ret;

	media_pipeline_for_each_entity(pipe, &iter, entity) {
		if (entity->obj_type == MEDIA_ENTITY_TYPE_VIDEO_DEVICE)
			n_video_devices++;
	}

	media_pipeline_entity_iter_cleanup(&iter);

	return n_video_devices == pipe->start_count;
}

static int rzv2h_ivc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rzv2h_ivc *ivc = vb2_get_drv_priv(q);
	struct media_pipeline *pipe;
	int ret;

	ret = pm_runtime_resume_and_get(ivc->dev);
	if (ret)
		goto err_return_buffers;

	ret = video_device_pipeline_alloc_start(&ivc->vdev.dev);
	if (ret) {
		dev_err(ivc->dev, "failed to start media pipeline\n");
		goto err_pm_runtime_put;
	}

	rzv2h_ivc_format_configure(ivc);

	ivc->buffers.sequence = 0;

	spin_lock(&ivc->spinlock);
	ivc->vvalid_ifp = 0;
	spin_unlock(&ivc->spinlock);

	pipe = video_device_pipeline(&ivc->vdev.dev);
	if (rzv2h_ivc_pipeline_ready(pipe)) {
		ret = media_pipeline_started(pipe);
		if (ret)
			goto err_stop_pipeline;

		media_jobs_run_jobs(ivc->sched);
	}

	return 0;

err_stop_pipeline:
	video_device_pipeline_stop(&ivc->vdev.dev);
err_pm_runtime_put:
	pm_runtime_put(ivc->dev);
err_return_buffers:
	rzv2h_ivc_return_buffers(ivc, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void rzv2h_ivc_stop_streaming(struct vb2_queue *q)
{
	struct rzv2h_ivc *ivc = vb2_get_drv_priv(q);
	struct media_pipeline *pipe;

	pipe = video_device_pipeline(&ivc->vdev.dev);
	if (rzv2h_ivc_pipeline_ready(pipe)) {
		media_pipeline_stopped(pipe);
		media_jobs_cancel_jobs(ivc->sched);
	}

	rzv2h_ivc_return_buffers(ivc, VB2_BUF_STATE_ERROR);
	video_device_pipeline_stop(&ivc->vdev.dev);
	pm_runtime_put_autosuspend(ivc->dev);
	pm_runtime_mark_last_busy(ivc->dev);
}

static const struct vb2_ops rzv2h_ivc_vb2_ops = {
	.queue_setup		= &rzv2h_ivc_queue_setup,
	.buf_queue		= &rzv2h_ivc_buf_queue,
	.buf_init		= &rzv2h_ivc_buf_init,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.start_streaming	= &rzv2h_ivc_start_streaming,
	.stop_streaming		= &rzv2h_ivc_stop_streaming,
};

static const struct rzv2h_ivc_format *
rzv2h_ivc_format_from_pixelformat(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rzv2h_ivc_formats); i++)
		if (fourcc == rzv2h_ivc_formats[i].fourcc)
			return &rzv2h_ivc_formats[i];

	return &rzv2h_ivc_formats[0];
}

static int rzv2h_ivc_enum_fmt_vid_out(struct file *file, void *fh,
				      struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(rzv2h_ivc_formats))
		return -EINVAL;

	f->pixelformat = rzv2h_ivc_formats[f->index].fourcc;
	return 0;
}

static int rzv2h_ivc_g_fmt_vid_out(struct file *file, void *fh,
				   struct v4l2_format *f)
{
	struct rzv2h_ivc *ivc = video_drvdata(file);

	f->fmt.pix = ivc->format.pix;

	return 0;
}

static void rzv2h_ivc_try_fmt(struct v4l2_pix_format *pix)
{
	const struct rzv2h_ivc_format *fmt;

	fmt = rzv2h_ivc_format_from_pixelformat(pix->pixelformat);
	pix->pixelformat = fmt->fourcc;

	pix->width = clamp(pix->width, RZV2H_IVC_MIN_WIDTH,
			   RZV2H_IVC_MAX_WIDTH);
	pix->height = clamp(pix->height, RZV2H_IVC_MIN_HEIGHT,
			    RZV2H_IVC_MAX_HEIGHT);

	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;

	v4l2_fill_pixfmt(pix, pix->pixelformat, pix->width, pix->height);
}

static void rzv2h_ivc_set_format(struct rzv2h_ivc *ivc,
				 struct v4l2_pix_format *pix)
{
	rzv2h_ivc_try_fmt(pix);
	ivc->format.pix = *pix;
	ivc->format.fmt = rzv2h_ivc_format_from_pixelformat(pix->pixelformat);
}

static int rzv2h_ivc_s_fmt_vid_out(struct file *file, void *fh,
				   struct v4l2_format *f)
{
	struct rzv2h_ivc *ivc = video_drvdata(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (vb2_is_busy(&ivc->vdev.vb2q))
		return -EBUSY;

	rzv2h_ivc_set_format(ivc, pix);

	return 0;
}

static int rzv2h_ivc_try_fmt_vid_out(struct file *file, void *fh,
				     struct v4l2_format *f)
{
	rzv2h_ivc_try_fmt(&f->fmt.pix);
	return 0;
}

static int rzv2h_ivc_querycap(struct file *file, void *fh,
			      struct v4l2_capability *cap)
{
	strscpy(cap->driver, "rzv2h-ivc", sizeof(cap->driver));
	strscpy(cap->card, "Renesas Input Video Control", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops rzv2h_ivc_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_vid_out = rzv2h_ivc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out = rzv2h_ivc_g_fmt_vid_out,
	.vidioc_s_fmt_vid_out = rzv2h_ivc_s_fmt_vid_out,
	.vidioc_try_fmt_vid_out = rzv2h_ivc_try_fmt_vid_out,
	.vidioc_querycap = rzv2h_ivc_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations rzv2h_ivc_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int rzv2h_ivc_populate_media_job(struct media_job *job, void *data)
{
	struct rzv2h_ivc *ivc = data;
	int ret;

	ret = media_jobs_add_job_dep(job, &rzv2h_ivc_media_job_dep_ops, ivc);
	if (ret)
		return ret;

	ret = media_jobs_add_job_step(job, rzv2h_ivc_job_set_next_buffer, ivc,
				      MEDIA_JOBS_FL_STEP_ANYWHERE, 0);
	if (ret)
		return ret;

	/*
	 * This stage will be the second to last one to run - the ISP driver may
	 * have some post-frame processing to do.
	 */
	ret = media_jobs_add_job_step(job, rzv2h_ivc_job_send_next_buffer, ivc,
				      MEDIA_JOBS_FL_STEP_FROM_BACK, 1);
	if (ret)
		return ret;

	return 0;
}

int rzv2h_initialise_video_dev_and_queue(struct rzv2h_ivc *ivc,
					 struct v4l2_device *v4l2_dev)
{
	struct v4l2_pix_format pix;
	struct video_device *vdev;
	struct vb2_queue *vb2q;
	int ret;

	spin_lock_init(&ivc->buffers.lock);
	INIT_LIST_HEAD(&ivc->buffers.queue);
	INIT_LIST_HEAD(&ivc->buffers.pending);
	init_waitqueue_head(&ivc->buffers.wq);

	/* Initialise vb2 queue */
	vb2q = &ivc->vdev.vb2q;
	vb2q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
	vb2q->drv_priv = ivc;
	vb2q->mem_ops = &vb2_dma_contig_memops;
	vb2q->ops = &rzv2h_ivc_vb2_ops;
	vb2q->buf_struct_size = sizeof(struct rzv2h_ivc_buf);
	vb2q->min_queued_buffers = 1;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2q->lock = &ivc->lock;
	vb2q->dev = ivc->dev;

	ret = vb2_queue_init(vb2q);
	if (ret)
		return dev_err_probe(ivc->dev, ret, "vb2 queue init failed\n");

	/* Initialise Video Device */
	vdev = &ivc->vdev.dev;
	strscpy(vdev->name, "rzv2h-ivc", sizeof(vdev->name));
	vdev->release = video_device_release_empty;
	vdev->fops = &rzv2h_ivc_v4l2_fops;
	vdev->ioctl_ops = &rzv2h_ivc_v4l2_ioctl_ops;
	vdev->lock = &ivc->lock;
	vdev->v4l2_dev = v4l2_dev;
	vdev->queue = vb2q;
	vdev->device_caps = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING;
	vdev->vfl_dir = VFL_DIR_TX;
	video_set_drvdata(vdev, ivc);

	memset(&pix, 0, sizeof(pix));
	pix.pixelformat = V4L2_PIX_FMT_SRGGB16;
	pix.width = RZV2H_IVC_DEFAULT_WIDTH;
	pix.height = RZV2H_IVC_DEFAULT_HEIGHT;
	rzv2h_ivc_set_format(ivc, &pix);

	ivc->vdev.pad.flags = MEDIA_PAD_FL_SOURCE;
	ivc->vdev.dev.entity.ops = &rzv2h_ivc_media_ops;
	ret = media_entity_pads_init(&ivc->vdev.dev.entity, 1, &ivc->vdev.pad);
	if (ret)
		goto err_release_vb2q;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(ivc->dev, "failed to register params video device\n");
		goto err_cleanup_vdev_entity;
	}

	ret = media_create_pad_link(&vdev->entity, 0, &ivc->subdev.sd.entity,
				    RZV2H_IVC_SUBDEV_SINK_PAD,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(ivc->dev, "failed to create media link\n");
		goto err_unregister_vdev;
	}

	ivc->sched = media_jobs_get_scheduler(vdev->entity.graph_obj.mdev);
	if (IS_ERR(ivc->sched)) {
		ret = PTR_ERR(ivc->sched);
		goto err_remove_link;
	}

	ret = media_jobs_add_job_setup_func(ivc->sched,
					    rzv2h_ivc_populate_media_job, ivc,
					    MEDIA_JOB_TYPE_PIPELINE_PULSE);
	if (ret)
		goto err_put_media_job_scheduler;

	return 0;

err_put_media_job_scheduler:
	media_jobs_put_scheduler(ivc->sched);
err_remove_link:
	media_entity_remove_links(&vdev->entity);
err_unregister_vdev:
	video_unregister_device(vdev);
err_cleanup_vdev_entity:
	media_entity_cleanup(&vdev->entity);
err_release_vb2q:
	vb2_queue_release(vb2q);

	return ret;
}

void rzv2h_deinit_video_dev_and_queue(struct rzv2h_ivc *ivc)
{
	struct video_device *vdev = &ivc->vdev.dev;
	struct vb2_queue *vb2q = &ivc->vdev.vb2q;

	media_jobs_put_scheduler(ivc->sched);
	vb2_video_unregister_device(vdev);
	media_entity_cleanup(&vdev->entity);
	vb2_queue_release(vb2q);
}
