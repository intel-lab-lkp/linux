// SPDX-License-Identifier: GPL-2.0+
/*
 * A virtual v4l2-mem2mem example for audio device.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>
#include <sound/dmaengine_pcm.h>

MODULE_DESCRIPTION("Virtual device for audio mem2mem testing");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");
MODULE_ALIAS("audio_mem2mem_testdev");

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "debug level");

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE	BIT(0)
#define MEM2MEM_OUTPUT	BIT(1)

#define MEM2MEM_NAME "audm2m"

#define dprintk(dev, lvl, fmt, arg...) \
	v4l2_dbg(lvl, debug, &(dev)->v4l2_dev, "%s: " fmt, __func__, ## arg)

#define SAMPLE_NUM 4096

static void audm2m_dev_release(struct device *dev)
{}

static struct platform_device audm2m_pdev = {
	.name		= MEM2MEM_NAME,
	.dev.release	= audm2m_dev_release,
};

struct audm2m_fmt {
	u32	fourcc;
	u32     types;
};

static struct audm2m_fmt formats[] = {
	{
		.fourcc	= V4L2_AUDIO_FMT_LPCM,
		.types  = MEM2MEM_CAPTURE | MEM2MEM_OUTPUT,
	}
};

#define NUM_FORMATS ARRAY_SIZE(formats)

/* Per-queue, driver-specific private data */
struct audm2m_q_data {
	unsigned int		rate;
	snd_pcm_format_t	format;
	unsigned int		channels;
	unsigned int		buffersize;
	struct audm2m_fmt	*fmt;
};

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

static struct audm2m_fmt *find_format(u32 fourcc)
{
	struct audm2m_fmt *fmt;
	unsigned int k;

	for (k = 0; k < NUM_FORMATS; k++) {
		fmt = &formats[k];
		if (fmt->fourcc == fourcc)
			break;
	}

	if (k == NUM_FORMATS)
		return NULL;

	return &formats[k];
}

struct audm2m_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	vfd;

	atomic_t		num_inst;
	struct mutex		dev_mutex;

	struct v4l2_m2m_dev	*m2m_dev;
};

struct audm2m_ctx {
	struct v4l2_fh		fh;
	struct audm2m_dev	*dev;

	struct mutex		vb_mutex;

	/* Abort requested by m2m */
	int			aborting;

	/* Source and destination queue data */
	struct audm2m_q_data   q_data[2];
};

static inline struct audm2m_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct audm2m_ctx, fh);
}

static struct audm2m_q_data *get_q_data(struct audm2m_ctx *ctx,
					enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_AUDIO_OUTPUT:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_AUDIO_CAPTURE:
		return &ctx->q_data[V4L2_M2M_DST];
	default:
		return NULL;
	}
}

static const char *type_name(enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_AUDIO_OUTPUT:
		return "Output";
	case V4L2_BUF_TYPE_AUDIO_CAPTURE:
		return "Capture";
	default:
		return "Invalid";
	}
}

/*
 * mem2mem callbacks
 */

/*
 * job_ready() - check whether an instance is ready to be scheduled to run
 */
static int job_ready(void *priv)
{
	struct audm2m_ctx *ctx = priv;

	if (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) < 1 ||
	    v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) < 1) {
		dprintk(ctx->dev, 1, "Not enough buffers available\n");
		return 0;
	}

	return 1;
}

static void job_abort(void *priv)
{
	struct audm2m_ctx *ctx = priv;

	/* Will cancel the transaction in the next interrupt handler */
	ctx->aborting = 1;
}

/*
 * device_run() - prepares and starts the device
 */
static void device_run(void *priv)
{
	struct audm2m_ctx *ctx = priv;
	struct audm2m_dev *audm2m_dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct audm2m_q_data *q_data_src, *q_data_dst;
	int src_size, dst_size;

	audm2m_dev = ctx->dev;

	q_data_src = get_q_data(ctx, V4L2_BUF_TYPE_AUDIO_OUTPUT);
	if (!q_data_src)
		return;

	q_data_dst = get_q_data(ctx, V4L2_BUF_TYPE_AUDIO_CAPTURE);
	if (!q_data_dst)
		return;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Process the conversion */
	src_size = vb2_get_plane_payload(&src_buf->vb2_buf, 0);

	if (src_size > q_data_dst->buffersize)
		dst_size = q_data_dst->buffersize;
	else
		dst_size = src_size;

	memcpy(vb2_plane_vaddr(&dst_buf->vb2_buf, 0),
	       vb2_plane_vaddr(&src_buf->vb2_buf, 0),
	       dst_size);

	vb2_set_plane_payload(&dst_buf->vb2_buf, 0, dst_size);

	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(audm2m_dev->m2m_dev, ctx->fh.m2m_ctx);
}

static int audm2m_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, MEM2MEM_NAME, sizeof(cap->driver));
	strscpy(cap->card, MEM2MEM_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", MEM2MEM_NAME);

	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, u32 type)
{
	int i, num;
	struct audm2m_fmt *fmt;

	num = 0;

	for (i = 0; i < NUM_FORMATS; ++i) {
		if (formats[i].types & type) {
			if (num == f->index)
				break;
			/*
			 * Correct type but haven't reached our index yet,
			 * just increment per-type index
			 */
			++num;
		}
	}

	if (i < NUM_FORMATS) {
		/* Format found */
		fmt = &formats[i];
		f->pixelformat = fmt->fourcc;
		return 0;
	}

	/* Format not found */
	return -EINVAL;
}

static int audm2m_enum_fmt_audio_cap(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_CAPTURE);
}

static int audm2m_enum_fmt_audio_out(struct file *file, void *priv,
				     struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_OUTPUT);
}

static int audm2m_g_fmt(struct audm2m_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct audm2m_q_data *q_data;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	f->fmt.audio.rate	= q_data->rate;
	f->fmt.audio.format	= q_data->format;
	f->fmt.audio.channels	= q_data->channels;
	f->fmt.audio.buffersize = q_data->buffersize;

	return 0;
}

static int audm2m_g_fmt_audio_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	return audm2m_g_fmt(file2ctx(file), f);
}

static int audm2m_g_fmt_audio_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	return audm2m_g_fmt(file2ctx(file), f);
}

static int audm2m_try_fmt(struct v4l2_format *f, struct audm2m_fmt *fmt)
{
	if (f->fmt.audio.rate < 8000)
		f->fmt.audio.rate = 8000;
	else if (f->fmt.audio.rate > 192000)
		f->fmt.audio.rate = 192000;

	if (f->fmt.audio.channels < 1)
		f->fmt.audio.channels = 1;
	else if (f->fmt.audio.channels > 8)
		f->fmt.audio.channels = 8;

	if (f->fmt.audio.format != SNDRV_PCM_FORMAT_S16_LE ||
	    f->fmt.audio.format != SNDRV_PCM_FORMAT_S32_LE)
		f->fmt.audio.format = SNDRV_PCM_FORMAT_S32_LE;

	f->fmt.audio.buffersize = f->fmt.audio.channels *
				  snd_pcm_format_physical_width(f->fmt.audio.format) *
				  SAMPLE_NUM;
	return 0;
}

static int audm2m_try_fmt_audio_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct audm2m_fmt *fmt;
	struct audm2m_ctx *ctx = file2ctx(file);

	fmt = find_format(f->fmt.pix.pixelformat);
	if (!fmt) {
		f->fmt.pix.pixelformat = formats[0].fourcc;
		fmt = find_format(f->fmt.pix.pixelformat);
	}

	if (!(fmt->types & MEM2MEM_CAPTURE)) {
		v4l2_err(&ctx->dev->v4l2_dev,
			 "Fourcc format (0x%08x) invalid.\n",
			 f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	return audm2m_try_fmt(f, fmt);
}

static int audm2m_try_fmt_audio_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct audm2m_fmt *fmt;
	struct audm2m_ctx *ctx = file2ctx(file);

	fmt = find_format(f->fmt.pix.pixelformat);
	if (!fmt) {
		f->fmt.pix.pixelformat = formats[0].fourcc;
		fmt = find_format(f->fmt.pix.pixelformat);
	}
	if (!(fmt->types & MEM2MEM_OUTPUT)) {
		v4l2_err(&ctx->dev->v4l2_dev,
			 "Fourcc format (0x%08x) invalid.\n",
			 f->fmt.pix.pixelformat);
		return -EINVAL;
	}

	return audm2m_try_fmt(f, fmt);
}

static int audm2m_s_fmt(struct audm2m_ctx *ctx, struct v4l2_format *f)
{
	struct audm2m_q_data *q_data;
	struct vb2_queue *vq;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&ctx->dev->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	q_data->fmt	= find_format(f->fmt.pix.pixelformat);
	q_data->rate	= f->fmt.audio.rate;
	q_data->format	= f->fmt.audio.format;
	q_data->channels = f->fmt.audio.channels;
	q_data->buffersize = q_data->channels *
			     snd_pcm_format_physical_width(q_data->format) *
			     SAMPLE_NUM;

	dprintk(ctx->dev, 1,
		"Format for type %s: %d/%d/%d, fmt: %c%c%c%c\n",
		type_name(f->type), q_data->rate, q_data->format,
		q_data->channels,
		(q_data->fmt->fourcc & 0xff),
		(q_data->fmt->fourcc >>  8) & 0xff,
		(q_data->fmt->fourcc >> 16) & 0xff,
		(q_data->fmt->fourcc >> 24) & 0xff);

	return 0;
}

static int audm2m_s_fmt_audio_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	int ret;

	ret = audm2m_try_fmt_audio_cap(file, priv, f);
	if (ret)
		return ret;

	return audm2m_s_fmt(file2ctx(file), f);
}

static int audm2m_s_fmt_audio_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	int ret;

	ret = audm2m_try_fmt_audio_out(file, priv, f);
	if (ret)
		return ret;

	return audm2m_s_fmt(file2ctx(file), f);
}

static const struct v4l2_ioctl_ops audm2m_ioctl_ops = {
	.vidioc_querycap		= audm2m_querycap,

	.vidioc_enum_fmt_audio_cap	= audm2m_enum_fmt_audio_cap,
	.vidioc_g_fmt_audio_cap		= audm2m_g_fmt_audio_cap,
	.vidioc_try_fmt_audio_cap	= audm2m_try_fmt_audio_cap,
	.vidioc_s_fmt_audio_cap		= audm2m_s_fmt_audio_cap,

	.vidioc_enum_fmt_audio_out	= audm2m_enum_fmt_audio_out,
	.vidioc_g_fmt_audio_out		= audm2m_g_fmt_audio_out,
	.vidioc_try_fmt_audio_out	= audm2m_try_fmt_audio_out,
	.vidioc_s_fmt_audio_out		= audm2m_s_fmt_audio_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/*
 * Queue operations
 */
static int audm2m_queue_setup(struct vb2_queue *vq,
			      unsigned int *nbuffers,
			      unsigned int *nplanes,
			      unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct audm2m_ctx *ctx = vb2_get_drv_priv(vq);
	struct audm2m_q_data *q_data;

	q_data = get_q_data(ctx, vq->type);
	if (!q_data)
		return -EINVAL;

	*nplanes = 1;
	sizes[0] = q_data->buffersize;

	dprintk(ctx->dev, 1, "%s: get %d buffer(s) of size %d each.\n",
		type_name(vq->type), *nplanes, sizes[0]);

	return 0;
}

static void audm2m_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct audm2m_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int audm2m_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct audm2m_ctx *ctx = vb2_get_drv_priv(q);
	struct audm2m_q_data *q_data = get_q_data(ctx, q->type);

	if (!q_data)
		return -EINVAL;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->aborting = 0;

	return 0;
}

static void audm2m_stop_streaming(struct vb2_queue *q)
{
	struct audm2m_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			return;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops audm2m_qops = {
	.queue_setup	 = audm2m_queue_setup,
	.buf_queue	 = audm2m_buf_queue,
	.start_streaming = audm2m_start_streaming,
	.stop_streaming  = audm2m_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct audm2m_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_AUDIO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &audm2m_qops;
	src_vq->mem_ops = &vb2_vmalloc_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->vb_mutex;
	src_vq->min_buffers_needed = 1;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_AUDIO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &audm2m_qops;
	dst_vq->mem_ops = &vb2_vmalloc_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->vb_mutex;
	dst_vq->min_buffers_needed = 1;

	return vb2_queue_init(dst_vq);
}

/*
 * File operations
 */
static int audm2m_open(struct file *file)
{
	struct audm2m_dev *dev = video_drvdata(file);
	struct audm2m_ctx *ctx = NULL;
	int rc = 0;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto open_unlock;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	ctx->q_data[V4L2_M2M_SRC].fmt = &formats[0];
	ctx->q_data[V4L2_M2M_SRC].rate = 8000;
	ctx->q_data[V4L2_M2M_SRC].format = SNDRV_PCM_FORMAT_S32_LE;
	ctx->q_data[V4L2_M2M_SRC].channels = 2;

	/* Fix to 4096 samples */
	ctx->q_data[V4L2_M2M_SRC].buffersize = SAMPLE_NUM * 2 * 4;
	ctx->q_data[V4L2_M2M_DST] = ctx->q_data[V4L2_M2M_SRC];

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);

	mutex_init(&ctx->vb_mutex);

	if (IS_ERR(ctx->fh.m2m_ctx)) {
		rc = PTR_ERR(ctx->fh.m2m_ctx);

		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		goto open_unlock;
	}

	v4l2_fh_add(&ctx->fh);
	atomic_inc(&dev->num_inst);

	dprintk(dev, 1, "Created instance: %p, m2m_ctx: %p\n",
		ctx, ctx->fh.m2m_ctx);

open_unlock:
	mutex_unlock(&dev->dev_mutex);
	return rc;
}

static int audm2m_release(struct file *file)
{
	struct audm2m_dev *dev = video_drvdata(file);
	struct audm2m_ctx *ctx = file2ctx(file);

	dprintk(dev, 1, "Releasing instance %p\n", ctx);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	mutex_lock(&dev->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&dev->dev_mutex);
	kfree(ctx);

	atomic_dec(&dev->num_inst);

	return 0;
}

static void audm2m_device_release(struct video_device *vdev)
{
	struct audm2m_dev *dev = container_of(vdev, struct audm2m_dev, vfd);

	v4l2_device_unregister(&dev->v4l2_dev);
	v4l2_m2m_release(dev->m2m_dev);

	kfree(dev);
}

static const struct v4l2_file_operations audm2m_fops = {
	.owner		= THIS_MODULE,
	.open		= audm2m_open,
	.release	= audm2m_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device audm2m_videodev = {
	.name		= MEM2MEM_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &audm2m_fops,
	.ioctl_ops	= &audm2m_ioctl_ops,
	.minor		= -1,
	.release	= audm2m_device_release,
	.device_caps	= V4L2_CAP_AUDIO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_ready	= job_ready,
	.job_abort	= job_abort,
};

static int audm2m_probe(struct platform_device *pdev)
{
	struct audm2m_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto error_free;

	atomic_set(&dev->num_inst, 0);
	mutex_init(&dev->dev_mutex);

	dev->vfd = audm2m_videodev;
	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	video_set_drvdata(vfd, dev);
	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/v4l-audio%d\n", vfd->num);

	platform_set_drvdata(pdev, dev);

	dev->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		dev->m2m_dev = NULL;
		goto error_dev;
	}

	ret = video_register_device(vfd, VFL_TYPE_AUDIO, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto error_m2m;
	}

	return 0;

error_m2m:
	v4l2_m2m_release(dev->m2m_dev);
error_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
error_free:
	kfree(dev);

	return ret;
}

static void audm2m_remove(struct platform_device *pdev)
{
	struct audm2m_dev *dev = platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " MEM2MEM_NAME);

	video_unregister_device(&dev->vfd);
}

static struct platform_driver audm2m_pdrv = {
	.probe		= audm2m_probe,
	.remove_new	= audm2m_remove,
	.driver		= {
		.name	= MEM2MEM_NAME,
	},
};

static void __exit audm2m_exit(void)
{
	platform_driver_unregister(&audm2m_pdrv);
	platform_device_unregister(&audm2m_pdev);
}

static int __init audm2m_init(void)
{
	int ret;

	ret = platform_device_register(&audm2m_pdev);
	if (ret)
		return ret;

	ret = platform_driver_register(&audm2m_pdrv);
	if (ret)
		platform_device_unregister(&audm2m_pdev);

	return ret;
}

module_init(audm2m_init);
module_exit(audm2m_exit);
