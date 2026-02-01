// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Jarkko Sakkinen 2025-2026
 *
 * Derived originally from v4l2loopback driver but is essentially a rewrite.
 */

/**
 * DOC: Theory of Operation
 *
 * The driver exposes /dev/vcam for creating virtual capture devices via
 * %VCAM_IOC_CREATE. The ioctl registers a video capture node and associates
 * output buffers described by &struct vcam_frame with DMA-BUF file descriptors
 * supplied by the caller. This also keeps output buffers owned by the caller,
 * and accounted from the calling process.
 *
 * Frames are pushed to the capture device by queueing output buffers using
 * %VCAM_IOC_QUEUE, and recycling them with %VCAM_IOC_DEQUEUE. Queueing without
 * dequeuing eventually exhausts the output queue and stalls the producer.
 *
 * If both buffers reference the same DMA-BUF, the driver performs a zero-copy
 * transfer by propagating metadata. Otherwise, if both buffers are mappable,
 * the payload is copied into the capture buffer. When neither zero-copy nor a
 * CPU mapping is possible, the capture buffer completes with an error.
 */

#include <linux/cleanup.h>
#include <linux/bitops.h>
#include <linux/atomic.h>
#include <linux/ctype.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/time.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include <uapi/linux/vcam.h>

#undef pr_fmt
#define pr_fmt(fmt) "vcam: " fmt

MODULE_DESCRIPTION("V4L2 virtual camera driver");
MODULE_LICENSE("GPL");

#define VCAM_CARD_LABEL_MAX sizeof_field(struct video_device, name)
#define VCAM_FPS_MIN 1
#define VCAM_FPS_MAX 1000

#define VCAM_MIN_WIDTH 2
#define VCAM_MIN_HEIGHT 2
#define VCAM_MAX_WIDTH 8192
#define VCAM_MAX_HEIGHT 8192
#define VCAM_DEFAULT_WIDTH 640
#define VCAM_DEFAULT_HEIGHT 480

#define VCAM_MAX_FORMATS 16
#define VCAM_MIN_FRAMES 2
#define VCAM_MAX_FRAMES 32

#define VCAM_STATUS_MASK (VCAM_STATUS_IDLE | VCAM_STATUS_STREAMING)

enum vcam_flags {
	VCAM_FLAG_IS_OPEN = 0x01,
	VCAM_FLAG_CREATING = 0x02,
	VCAM_FLAG_READY = 0x04,
};

struct vcam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	unsigned long flags;
};

enum vcam_buf_flags {
	VCAM_BUF_FLAG_MAPPABLE = BIT(0),
};

struct vcam {
	unsigned long flags;
	int device_nr;
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct vb2_queue capture_queue;
	struct vb2_queue output_queue;
	struct v4l2_pix_format pix_format;
	struct v4l2_captureparm capture;
	atomic_t sequence;
	struct list_head capture_list;
	struct list_head output_list;
	u64 status;
	wait_queue_head_t status_waitq;
	enum vb2_memory output_memory;

	/* Protects status flags and wait queue updates. */
	spinlock_t status_lock;

	/* Shared lock for vdev and VB2 queues. */
	struct mutex lock;

	/* Protects capture_list and output_list. */
	spinlock_t frame_lock;

	/*
	 * Maintains a shared reference between processes having either
	 * /dev/vcam or /dev/videoX open.
	 */
	struct kref ref;
};

enum vcam_format_flags {
	VCAM_PLANAR = BIT(0),
	VCAM_COMPRESSED = BIT(1),
};

struct vcam_format {
	int fourcc;
	int depth;
	int flags;
};

const struct vcam_format vcam_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.depth = 16,
		.flags = 0,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.depth = 12,
		.flags = VCAM_PLANAR,
	},
	{
		.fourcc = V4L2_PIX_FMT_MJPEG,
		.depth = 32,
		.flags = VCAM_COMPRESSED,
	},
};

#define VCAM_NR_FORMATS ARRAY_SIZE(vcam_formats)

static const struct vcam_format *vcam_find_format(int fourcc)
{
	unsigned int i;

	for (i = 0; i < VCAM_NR_FORMATS; i++) {
		if (vcam_formats[i].fourcc == fourcc)
			return vcam_formats + i;
	}

	return NULL;
}

static void vcam_fmt_descr(char *dst, size_t dst_len, u32 format)
{
	snprintf(dst, dst_len, "[%c%c%c%c]", (format >> 0) & 0xFF,
		 (format >> 8) & 0xFF, (format >> 16) & 0xFF,
		 (format >> 24) & 0xFF);
}

static void vcam_fourcc_str(char *dst, u32 format)
{
	dst[0] = (format >> 0) & 0xFF;
	dst[1] = (format >> 8) & 0xFF;
	dst[2] = (format >> 16) & 0xFF;
	dst[3] = (format >> 24) & 0xFF;
	dst[4] = '\0';
}

static inline bool vcam_is_streaming(struct vcam *data)
{
	return vb2_is_streaming(&data->output_queue) ||
	       vb2_is_streaming(&data->capture_queue);
}

static bool vcam_status_mask_ready(struct vcam *dev, u64 mask)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&dev->status_lock, flags);
	ready = (dev->status & mask) == mask;
	spin_unlock_irqrestore(&dev->status_lock, flags);

	return ready;
}

static void vcam_status_update_stream(struct vcam *dev, bool on)
{
	unsigned long flags;
	u64 old_flags;
	u64 new_flags;

	spin_lock_irqsave(&dev->status_lock, flags);
	old_flags = dev->status;
	if (on) {
		dev->status &= ~VCAM_STATUS_IDLE;
		dev->status |= VCAM_STATUS_STREAMING;
	} else {
		dev->status &= ~VCAM_STATUS_STREAMING;
		dev->status |= VCAM_STATUS_IDLE;
	}
	new_flags = dev->status;
	spin_unlock_irqrestore(&dev->status_lock, flags);

	if (new_flags != old_flags)
		wake_up_interruptible(&dev->status_waitq);
}

static u64 vcam_status_read(struct vcam *dev)
{
	unsigned long flags;
	u64 flags_snapshot;

	spin_lock_irqsave(&dev->status_lock, flags);
	flags_snapshot = dev->status;
	spin_unlock_irqrestore(&dev->status_lock, flags);

	return flags_snapshot;
}

static bool vcam_tpf_valid(const struct v4l2_fract *tpf)
{
	u64 min_den = (u64)tpf->numerator * VCAM_FPS_MIN;
	u64 max_den = (u64)tpf->numerator * VCAM_FPS_MAX;

	if (!tpf->numerator || !tpf->denominator)
		return false;
	if ((u64)tpf->denominator < min_den)
		return false;
	if ((u64)tpf->denominator > max_den)
		return false;

	return true;
}

static bool vcam_pix_format_eq(const struct v4l2_pix_format *src,
			       const struct v4l2_pix_format *dest)
{
	return src->width == dest->width && src->height == dest->height &&
	       src->pixelformat == dest->pixelformat;
}

static int vcam_set_format(struct vcam *dev, struct v4l2_format *fmt)
{
	struct v4l2_pix_format *pix = &fmt->fmt.pix;
	const struct vcam_format *format;
	u64 bytesperline;
	u64 sizeimage;

	if (V4L2_TYPE_IS_MULTIPLANAR(fmt->type))
		return -EINVAL;

	if (!pix->width)
		pix->width = VCAM_DEFAULT_WIDTH;
	if (!pix->height)
		pix->height = VCAM_DEFAULT_HEIGHT;

	pix->width = clamp(pix->width, VCAM_MIN_WIDTH, VCAM_MAX_WIDTH);
	pix->height = clamp(pix->height, VCAM_MIN_HEIGHT, VCAM_MAX_HEIGHT);

	format = vcam_find_format(pix->pixelformat);
	if (!format) {
		format = &vcam_formats[0];
		pix->pixelformat = format->fourcc;
	}

	if (format->flags & VCAM_PLANAR) {
		pix->bytesperline = pix->width;
		sizeimage = ((u64)pix->width * pix->height * format->depth) >>
			    3;
	} else if (format->flags & VCAM_COMPRESSED) {
		pix->bytesperline = 0;
		sizeimage = ((u64)pix->width * pix->height * format->depth) >>
			    3;
	} else {
		bytesperline = ((u64)pix->width * format->depth) >> 3;
		if (bytesperline > U32_MAX)
			return -EOVERFLOW;

		pix->bytesperline = bytesperline;
		sizeimage = (u64)pix->height * bytesperline;
	}

	if (sizeimage > U32_MAX)
		return -EOVERFLOW;

	pix->sizeimage = sizeimage;

	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT ||
	    pix->colorspace > V4L2_COLORSPACE_DCI_P3)
		pix->colorspace = V4L2_COLORSPACE_SRGB;
	if (pix->field == V4L2_FIELD_ANY)
		pix->field = V4L2_FIELD_NONE;

	return 0;
}

static int vcam_vidioc_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	__u32 capabilities = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_CAPTURE;
	struct vcam *dev = video_drvdata(file);

	cap->device_caps = capabilities;
	cap->capabilities = capabilities | V4L2_CAP_DEVICE_CAPS;

	strscpy(cap->driver, "vcam", sizeof(cap->driver));
	strscpy(cap->card, dev->vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "vcam:%d",
		 dev->device_nr);

	return 0;
}

static int vcam_enum_framesizes(struct vcam *dev, struct v4l2_frmsizeenum *argp)
{
	if (argp->index)
		return -EINVAL;

	if (vcam_is_streaming(dev)) {
		if (argp->pixel_format != dev->pix_format.pixelformat)
			return -EINVAL;

		argp->type = V4L2_FRMSIZE_TYPE_DISCRETE;

		argp->discrete.width = dev->pix_format.width;
		argp->discrete.height = dev->pix_format.height;
	} else {
		if (!vcam_find_format(argp->pixel_format))
			return -EINVAL;

		argp->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;

		argp->stepwise.min_width = VCAM_MIN_WIDTH;
		argp->stepwise.min_height = VCAM_MIN_HEIGHT;
		argp->stepwise.max_width = VCAM_MAX_WIDTH;
		argp->stepwise.max_height = VCAM_MAX_HEIGHT;
		argp->stepwise.step_width = 1;
		argp->stepwise.step_height = 1;
	}

	return 0;
}

static int vcam_enum_frameintervals(struct vcam *dev,
				    struct v4l2_frmivalenum *argp)
{
	if (argp->index)
		return -EINVAL;

	if (vcam_is_streaming(dev)) {
		if (argp->width != dev->pix_format.width ||
		    argp->height != dev->pix_format.height ||
		    argp->pixel_format != dev->pix_format.pixelformat)
			return -EINVAL;

		argp->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		argp->discrete = dev->capture.timeperframe;
	} else {
		if (argp->width < VCAM_MIN_WIDTH ||
		    argp->width > VCAM_MAX_WIDTH ||
		    argp->height < VCAM_MIN_HEIGHT ||
		    argp->height > VCAM_MAX_HEIGHT ||
		    !vcam_find_format(argp->pixel_format))
			return -EINVAL;

		argp->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
		argp->stepwise.min.numerator = 1;
		argp->stepwise.min.denominator = VCAM_FPS_MAX;
		argp->stepwise.max.numerator = 1;
		argp->stepwise.max.denominator = VCAM_FPS_MIN;
		argp->stepwise.step.numerator = 1;
		argp->stepwise.step.denominator = 1;
	}

	return 0;
}

static int vcam_vidioc_enum_framesizes(struct file *file, void *fh,
				       struct v4l2_frmsizeenum *argp)
{
	struct vcam *dev = video_drvdata(file);

	return vcam_enum_framesizes(dev, argp);
}

static int vcam_vidioc_enum_frameintervals(struct file *file, void *fh,
					   struct v4l2_frmivalenum *argp)
{
	struct vcam *dev = video_drvdata(file);

	return vcam_enum_frameintervals(dev, argp);
}

static int vcam_vidioc_enum_fmt_cap(struct file *file, void *fh,
				    struct v4l2_fmtdesc *f)
{
	struct vcam *dev;

	dev = video_drvdata(file);

	if (vcam_is_streaming(dev)) {
		const __u32 format = dev->pix_format.pixelformat;

		if (f->index)
			return -EINVAL;

		f->pixelformat = dev->pix_format.pixelformat;
		vcam_fmt_descr(f->description, sizeof(f->description), format);
	} else {
		if (f->index >= VCAM_NR_FORMATS)
			return -EINVAL;

		f->pixelformat = vcam_formats[f->index].fourcc;
		vcam_fmt_descr(f->description, sizeof(f->description),
			       f->pixelformat);
	}
	f->flags = 0;
	return 0;
}

static int vcam_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *fmt)
{
	struct vcam *dev;

	dev = video_drvdata(file);

	fmt->fmt.pix = dev->pix_format;
	return 0;
}

static int vcam_vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				       struct v4l2_format *fmt)
{
	struct vcam *dev = video_drvdata(file);

	if (!V4L2_TYPE_IS_CAPTURE(fmt->type))
		return -EINVAL;

	if (vcam_is_streaming(dev)) {
		if (!vcam_pix_format_eq(&dev->pix_format, &fmt->fmt.pix))
			return -EBUSY;

		fmt->fmt.pix = dev->pix_format;
	}

	return vcam_set_format(dev, fmt);
}

static int vcam_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *fmt)
{
	struct vcam *dev = video_drvdata(file);
	struct v4l2_format try_fmt = *fmt;
	int ret;

	if (!V4L2_TYPE_IS_CAPTURE(fmt->type))
		return -EINVAL;

	if (vcam_is_streaming(dev)) {
		if (!vcam_pix_format_eq(&dev->pix_format, &fmt->fmt.pix))
			return -EBUSY;

		fmt->fmt.pix = dev->pix_format;
	}

	ret = vcam_set_format(dev, &try_fmt);
	if (ret)
		return ret;

	if (vb2_is_busy(&dev->output_queue) &&
	    !vcam_pix_format_eq(&dev->pix_format, &try_fmt.fmt.pix))
		return -EBUSY;

	dev->pix_format = try_fmt.fmt.pix;
	*fmt = try_fmt;
	return 0;
}

static int vcam_ioc_reqbufs(struct file *file, struct vcam *dev,
			    struct v4l2_requestbuffers *req)
{
	int ret = 0;

	if (req->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	scoped_guard(mutex, &dev->lock)
	{
		if (vb2_queue_is_busy(&dev->output_queue, file)) {
			ret = -EBUSY;
			break;
		}

		ret = vb2_reqbufs(&dev->output_queue, req);
		if (!ret)
			dev->output_queue.owner =
				req->count ? file->private_data : NULL;
	}
	return ret;
}

static int vcam_ioc_querybuf(struct file *file, struct vcam *dev,
			     struct v4l2_buffer *buf)
{
	int ret = 0;

	if (buf->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	scoped_guard(mutex, &dev->lock)
		ret = vb2_querybuf(&dev->output_queue, buf);

	return ret;
}

static ssize_t formats_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct vcam_format_entry {
		u32 fourcc;
		char name[5];
	};
	struct vcam_format_entry formats[VCAM_MAX_FORMATS];
	struct vcam_format_entry tmp;
	unsigned int count =
		min_t(unsigned int, VCAM_NR_FORMATS, VCAM_MAX_FORMATS);
	size_t len = 0;
	unsigned int i, j;

	for (i = 0; i < count; i++) {
		formats[i].fourcc = vcam_formats[i].fourcc;
		vcam_fourcc_str(formats[i].name, formats[i].fourcc);
	}

	for (i = 1; i < count; i++) {
		for (j = i; j > 0; j--) {
			if (strcmp(formats[j - 1].name, formats[j].name) <= 0)
				break;
			tmp = formats[j - 1];
			formats[j - 1] = formats[j];
			formats[j] = tmp;
		}
	}

	for (i = 0; i < count; i++)
		len += sysfs_emit_at(buf, len, "%s%s", i ? " " : "",
				     formats[i].name);

	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t max_width_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%u\n", VCAM_MAX_WIDTH);
}

static ssize_t max_height_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", VCAM_MAX_HEIGHT);
}

static ssize_t max_frames_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", VCAM_MAX_FRAMES);
}

static DEVICE_ATTR_RO(formats);
static DEVICE_ATTR_RO(max_frames);
static DEVICE_ATTR_RO(max_height);
static DEVICE_ATTR_RO(max_width);

static struct attribute *vcam_attrs[] = {
	&dev_attr_formats.attr,
	&dev_attr_max_frames.attr,
	&dev_attr_max_height.attr,
	&dev_attr_max_width.attr,
	NULL,
};

static const struct attribute_group vcam_attr_group = {
	.attrs = vcam_attrs,
};

static const struct attribute_group *vcam_attr_groups[] = {
	&vcam_attr_group,
	NULL,
};

static int vcam_ioc_alloc(struct file *file, struct vcam *dev, u32 nr_frames,
			  void __user *frames_user, enum vb2_memory memory)
{
	struct v4l2_requestbuffers req = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = memory,
	};
	struct v4l2_buffer buf;
	struct vcam_frame *frames = NULL;
	unsigned int i;
	int ret;

	if (memory == VB2_MEMORY_DMABUF &&
	    !dev->output_queue.mem_ops->attach_dmabuf)
		return -EOPNOTSUPP;

	if (!frames_user)
		return -EINVAL;

	if (nr_frames) {
		frames = kcalloc(nr_frames, sizeof(*frames), GFP_KERNEL);
		if (!frames)
			return -ENOMEM;
	}

	if (copy_from_user(frames, frames_user, nr_frames * sizeof(*frames))) {
		ret = -EFAULT;
		goto out_free;
	}

	req.count = nr_frames;
	ret = vcam_ioc_reqbufs(file, dev, &req);
	if (ret)
		goto out_free;

	if (req.count != nr_frames) {
		struct v4l2_requestbuffers req_free = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = memory,
			.count = 0,
		};

		vcam_ioc_reqbufs(file, dev, &req_free);
		ret = -ENOMEM;
		goto out_free;
	}

	dev->output_memory = memory;

	for (i = 0; i < nr_frames; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buf.memory = memory;
		buf.index = i;

		ret = vcam_ioc_querybuf(file, dev, &buf);
		if (ret)
			goto out_free_reqbufs;

		frames[i].index = i;
		frames[i].length = buf.length;
	}

	if (copy_to_user(frames_user, frames, nr_frames * sizeof(*frames)))
		ret = -EFAULT;

out_free_reqbufs:
	if (ret) {
		struct v4l2_requestbuffers req_free = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = memory,
			.count = 0,
		};

		vcam_ioc_reqbufs(file, dev, &req_free);
		dev->output_memory = VB2_MEMORY_DMABUF;
	}
out_free:
	kfree(frames);
	if (ret)
		return ret;

	return 0;
}

static int vcam_ioc_queue(struct file *file, struct vcam *dev,
			  struct vcam_ioc_queue *queue)
{
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = dev->output_memory,
		.index = queue->index,
		.bytesused = queue->length,
	};
	u32 remainder;
	int ret;

	if (queue->reserved)
		return -EINVAL;

	if (dev->output_memory == VB2_MEMORY_DMABUF) {
		buf.m.fd = queue->fd;
		buf.length = dev->pix_format.sizeimage;
	}

	buf.timestamp.tv_sec =
		div_u64_rem(queue->timestamp, NSEC_PER_SEC, &remainder);
	buf.timestamp.tv_usec = remainder / NSEC_PER_USEC;

	scoped_guard(mutex, &dev->lock)
	{
		if (vb2_queue_is_busy(&dev->output_queue, file)) {
			ret = -EBUSY;
			break;
		}

		if (vb2_is_streaming(&dev->capture_queue) &&
		    !vb2_is_streaming(&dev->output_queue)) {
			ret = vb2_streamon(&dev->output_queue, buf.type);
			if (ret)
				break;
		}

		ret = vb2_qbuf(&dev->output_queue, dev->v4l2_dev.mdev, &buf);
	}

	return ret;
}

static int vcam_ioc_dequeue(struct file *file, struct vcam *dev,
			    struct vcam_ioc_dequeue *queue)
{
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = dev->output_memory,
	};
	int ret;

	scoped_guard(mutex, &dev->lock)
	{
		if (vb2_queue_is_busy(&dev->output_queue, file)) {
			ret = -EBUSY;
			break;
		}

		ret = vb2_dqbuf(&dev->output_queue, &buf,
				file->f_flags & O_NONBLOCK);
	}
	if (ret)
		return ret;

	queue->index = buf.index;
	queue->length = buf.bytesused;
	queue->timestamp = (u64)buf.timestamp.tv_sec * NSEC_PER_SEC +
			   (u64)buf.timestamp.tv_usec * NSEC_PER_USEC;
	return 0;
}

static int vcam_ioc_status(struct vcam *dev, __u64 *status)
{
	*status = vcam_status_read(dev);
	return 0;
}

static int vcam_ioc_wait(struct vcam *dev, struct vcam_ioc_wait *wait)
{
	int ret;

	if (!wait->mask)
		return -EINVAL;
	if (wait->mask & ~VCAM_STATUS_MASK)
		return -EINVAL;

	ret = wait_event_interruptible(dev->status_waitq,
				       vcam_status_mask_ready(dev, wait->mask));
	if (ret)
		return ret;

	wait->status = vcam_status_read(dev);
	return 0;
}

static long vcam_output_ioctl_core(struct file *file, unsigned int cmd,
				   void *arg)
{
	struct vcam *dev = file->private_data;
	long ret = 0;

	switch (cmd) {
	case VCAM_IOC_QUEUE:
		ret = vcam_ioc_queue(file, dev, arg);
		break;
	case VCAM_IOC_DEQUEUE:
		ret = vcam_ioc_dequeue(file, dev, arg);
		break;
	case VCAM_IOC_STATUS:
		ret = vcam_ioc_status(dev, arg);
		break;
	case VCAM_IOC_WAIT:
		ret = vcam_ioc_wait(dev, arg);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static long vcam_ioctl_common(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	void *karg;
	size_t size;
	long ret;

	switch (cmd) {
	case VCAM_IOC_QUEUE:
		size = sizeof(struct vcam_ioc_queue);
		break;
	case VCAM_IOC_DEQUEUE:
		size = sizeof(struct vcam_ioc_dequeue);
		break;
	case VCAM_IOC_STATUS:
		size = sizeof(__u64);
		break;
	case VCAM_IOC_WAIT:
		size = sizeof(struct vcam_ioc_wait);
		break;
	default:
		return -ENOTTY;
	}

	if (size > SZ_4K)
		return -ENOTTY;

	karg = kzalloc(size, GFP_KERNEL);
	if (!karg)
		return -ENOMEM;

	if (copy_from_user(karg, argp, size)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = vcam_output_ioctl_core(file, cmd, karg);
	if (ret)
		goto out_free;

	if (copy_to_user(argp, karg, size)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = 0;
out_free:
	kfree(karg);
	return ret;
}

static void __vcam_release(struct vcam *dev)
{
	if (!dev->vdev)
		return;

	vb2_queue_release(&dev->output_queue);
	vb2_queue_release(&dev->capture_queue);

	if (video_is_registered(dev->vdev))
		video_unregister_device(dev->vdev);
	else
		video_device_release(dev->vdev);

	v4l2_device_unregister(&dev->v4l2_dev);

	dev->vdev = NULL;
	dev->device_nr = -1;
}

static void vcam_release(struct kref *ref)
{
	struct vcam *dev;

	dev = container_of(ref, struct vcam, ref);

	if (!test_bit(VCAM_FLAG_CREATING, &dev->flags) || dev->device_nr < 0) {
		kfree(dev);
		return;
	}

	__vcam_release(dev);
	kfree(dev);
}

static int __vcam_close(struct inode *inode, struct file *file)
{
	struct vcam *dev = file->private_data;

	if (dev->vdev && video_is_registered(dev->vdev))
		video_unregister_device(dev->vdev);

	vb2_queue_release(&dev->output_queue);

	dev->output_memory = VB2_MEMORY_DMABUF;

	kref_put(&dev->ref, vcam_release);
	return 0;
}

static int vcam_open(struct inode *inode, struct file *file)
{
	struct vcam *dev;
	int ret = nonseekable_open(inode, file);

	if (ret)
		return ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->ref);
	dev->device_nr = -1;
	file->private_data = dev;
	return 0;
}

static int vcam_close(struct inode *inode, struct file *file)
{
	struct vcam *dev = file->private_data;
	int ret = 0;

	if (!dev)
		return 0;

	if (test_bit(VCAM_FLAG_CREATING, &dev->flags) && dev->device_nr >= 0)
		ret = __vcam_close(inode, file);
	else
		kref_put(&dev->ref, vcam_release);

	file->private_data = NULL;
	return ret;
}

static __poll_t vcam_poll(struct file *file, struct poll_table_struct *pts)
{
	struct vcam *dev = file->private_data;

	if (!dev || !test_bit(VCAM_FLAG_CREATING, &dev->flags) ||
	    !test_bit(VCAM_FLAG_READY, &dev->flags) || dev->device_nr < 0)
		return POLLERR;

	return vb2_core_poll(&dev->output_queue, file, pts);
}

static int vcam_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vcam *dev = file->private_data;

	if (!dev || !test_bit(VCAM_FLAG_CREATING, &dev->flags) ||
	    !test_bit(VCAM_FLAG_READY, &dev->flags) || dev->device_nr < 0)
		return -ENOTTY;

	return vb2_mmap(&dev->output_queue, vma);
}

static int vcam_vidioc_g_parm(struct file *file, void *priv,
			      struct v4l2_streamparm *parm)
{
	struct vcam *dev;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	dev = video_drvdata(file);
	parm->parm.capture = dev->capture;
	return 0;
}

static int vcam_vidioc_s_parm(struct file *file, void *priv,
			      struct v4l2_streamparm *parm)
{
	struct v4l2_fract *tpf = &parm->parm.capture.timeperframe;
	struct vcam *dev = video_drvdata(file);

	if (!vcam_tpf_valid(tpf))
		return -EINVAL;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	dev->capture.timeperframe = *tpf;
	parm->parm.capture = dev->capture;
	return 0;
}

static int vcam_vidioc_enum_input(struct file *file, void *fh,
				  struct v4l2_input *inp)
{
	struct vcam *dev;
	__u32 index = inp->index;

	if (index != 0)
		return -EINVAL;

	memset(inp, 0, sizeof(*inp));

	inp->index = index;
	strscpy(inp->name, "vcam", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->audioset = 0;
	inp->tuner = 0;
	inp->status = 0;

	dev = video_drvdata(file);
	if (!vb2_is_streaming(&dev->output_queue))
		inp->status |= V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

static int vcam_vidioc_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vcam_vidioc_s_input(struct file *file, void *fh, unsigned int i)
{
	if (i == 0)
		return 0;

	return -EINVAL;
}

static int vcam_vidioc_streamon(struct file *file, void *fh,
				enum v4l2_buf_type type)
{
	struct vcam *dev = video_drvdata(file);
	int ret;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_queue_is_busy(&dev->capture_queue, file))
		return -EBUSY;

	ret = vb2_streamon(&dev->capture_queue, type);
	if (ret)
		return ret;

	if (vb2_get_num_buffers(&dev->output_queue)) {
		ret = vb2_streamon(&dev->output_queue,
				   V4L2_BUF_TYPE_VIDEO_OUTPUT);
		if (ret) {
			vb2_streamoff(&dev->capture_queue, type);
			return ret;
		}
	}

	return 0;
}

static int vcam_vidioc_streamoff(struct file *file, void *fh,
				 enum v4l2_buf_type type)
{
	struct vcam *dev = video_drvdata(file);
	int ret;

	if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (vb2_queue_is_busy(&dev->capture_queue, file))
		return -EBUSY;

	ret = vb2_streamoff(&dev->capture_queue, type);
	if (ret)
		return ret;

	if (vb2_get_num_buffers(&dev->output_queue))
		vb2_streamoff(&dev->output_queue, V4L2_BUF_TYPE_VIDEO_OUTPUT);

	return 0;
}

static const struct v4l2_ioctl_ops vcam_ioctl_ops = {
	.vidioc_querycap = &vcam_vidioc_querycap,
	.vidioc_enum_framesizes = &vcam_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = &vcam_vidioc_enum_frameintervals,
	.vidioc_enum_input = &vcam_vidioc_enum_input,
	.vidioc_g_input = &vcam_vidioc_g_input,
	.vidioc_s_input = &vcam_vidioc_s_input,
	.vidioc_enum_fmt_vid_cap = &vcam_vidioc_enum_fmt_cap,
	.vidioc_g_fmt_vid_cap = &vcam_vidioc_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = &vcam_vidioc_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = &vcam_vidioc_try_fmt_vid_cap,
	.vidioc_g_parm = &vcam_vidioc_g_parm,
	.vidioc_s_parm = &vcam_vidioc_s_parm,

	.vidioc_reqbufs = &vb2_ioctl_reqbufs,
	.vidioc_create_bufs = &vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = &vb2_ioctl_prepare_buf,
	.vidioc_querybuf = &vb2_ioctl_querybuf,
	.vidioc_qbuf = &vb2_ioctl_qbuf,
	.vidioc_dqbuf = &vb2_ioctl_dqbuf,
	.vidioc_expbuf = &vb2_ioctl_expbuf,
	.vidioc_streamon = &vcam_vidioc_streamon,
	.vidioc_streamoff = &vcam_vidioc_streamoff,
};

static enum vb2_buffer_state vcam_buf_fill(struct vcam *dev,
					   struct vcam_buf *buf,
					   const void *src, u32 src_len,
					   u64 timestamp)
{
	struct vb2_buffer *vb = &buf->vb.vb2_buf;
	u32 sequence;
	void *dst;

	dst = vb2_plane_vaddr(vb, 0);
	if (!dst)
		return VB2_BUF_STATE_ERROR;

	if (!src_len || src_len > dev->pix_format.sizeimage)
		src_len = dev->pix_format.sizeimage;

	if (!src)
		return VB2_BUF_STATE_ERROR;

	memcpy(dst, src, src_len);

	sequence = (u32)(atomic_inc_return(&dev->sequence) - 1);

	vb->timestamp = timestamp ? timestamp : ktime_get_ns();
	buf->vb.sequence = sequence;
	buf->vb.field = dev->pix_format.field;
	vb2_set_plane_payload(vb, 0, src_len);

	return VB2_BUF_STATE_DONE;
}

static bool vcam_buf_flip(struct vcam *dev, struct vb2_buffer *out_vb,
			  struct vcam_buf *cap_buf, u32 bytesused)
{
	struct vb2_buffer *cap_vb = &cap_buf->vb.vb2_buf;
	u32 sequence;

	if (!out_vb->planes[0].dbuf || !cap_vb->planes[0].dbuf)
		return false;

	if (out_vb->planes[0].dbuf != cap_vb->planes[0].dbuf)
		return false;

	if (!bytesused)
		bytesused = dev->pix_format.sizeimage;
	if (bytesused > vb2_plane_size(cap_vb, 0))
		bytesused = vb2_plane_size(cap_vb, 0);

	sequence = (u32)(atomic_inc_return(&dev->sequence) - 1);

	cap_vb->timestamp = out_vb->timestamp ? out_vb->timestamp :
						ktime_get_ns();
	cap_buf->vb.sequence = sequence;
	cap_buf->vb.field = dev->pix_format.field;
	vb2_set_plane_payload(cap_vb, 0, bytesused);

	return true;
}

static bool vcam_buf_pair_dequeue(struct vcam *dev, struct vcam_buf **out_buf,
				  struct vcam_buf **cap_buf)
{
	unsigned long flags;
	bool dequeued = false;

	spin_lock_irqsave(&dev->frame_lock, flags);
	if (!list_empty(&dev->output_list) && !list_empty(&dev->capture_list)) {
		*out_buf = list_first_entry(&dev->output_list, struct vcam_buf,
					    list);
		list_del(&(*out_buf)->list);
		*cap_buf = list_first_entry(&dev->capture_list, struct vcam_buf,
					    list);
		list_del(&(*cap_buf)->list);
		dequeued = true;
	}
	spin_unlock_irqrestore(&dev->frame_lock, flags);
	return dequeued;
}

static void vcam_dequeue_frames(struct vcam *data)
{
	const struct vcam_format *format;
	enum vb2_buffer_state cap_state;
	struct vcam_buf *cap_buf;
	struct vcam_buf *out_buf;
	struct vb2_buffer *vb;
	bool zero_copy;
	u32 bytesused;
	void *src;

	if (!vcam_is_streaming(data))
		return;

	format = vcam_find_format(data->pix_format.pixelformat);
	while (vcam_buf_pair_dequeue(data, &out_buf, &cap_buf)) {
		cap_state = VB2_BUF_STATE_DONE;
		vb = &out_buf->vb.vb2_buf;
		bytesused = vb2_get_plane_payload(vb, 0);
		if (!bytesused || bytesused > data->pix_format.sizeimage)
			bytesused = data->pix_format.sizeimage;

		if (bytesused < data->pix_format.sizeimage &&
		    (!format || !(format->flags & VCAM_COMPRESSED))) {
			cap_state = VB2_BUF_STATE_ERROR;
			goto out_done;
		}

		zero_copy = vcam_buf_flip(data, vb, cap_buf, bytesused);
		if (!zero_copy &&
		    (!(out_buf->flags & VCAM_BUF_FLAG_MAPPABLE) ||
		     !(cap_buf->flags & VCAM_BUF_FLAG_MAPPABLE))) {
			dev_dbg(&data->vdev->dev,
				"unshared unmappable capture and output");
			cap_state = VB2_BUF_STATE_ERROR;
			goto out_done;
		}
		if (!zero_copy) {
			src = vb2_plane_vaddr(vb, 0);
			if (!src) {
				cap_state = VB2_BUF_STATE_ERROR;
				goto out_done;
			}

			cap_state = vcam_buf_fill(data, cap_buf, src, bytesused,
						  vb->timestamp);
		}
out_done:
		vb2_buffer_done(&cap_buf->vb.vb2_buf, cap_state);

		if (cap_state == VB2_BUF_STATE_ERROR)
			vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		else
			vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
	}
}

static int vcam_vdev_open(struct file *file)
{
	struct vcam *dev;
	int ret;

	dev = video_drvdata(file);
	if (test_and_set_bit(VCAM_FLAG_IS_OPEN, &dev->flags))
		return -EBUSY;
	if (dev->device_nr < 0 || !test_bit(VCAM_FLAG_READY, &dev->flags)) {
		clear_bit(VCAM_FLAG_IS_OPEN, &dev->flags);
		return -ENODEV;
	}

	ret = v4l2_fh_open(file);
	if (ret) {
		clear_bit(VCAM_FLAG_IS_OPEN, &dev->flags);
		return ret;
	}

	kref_get(&dev->ref);
	return 0;
}

static int vcam_vdev_close(struct file *file)
{
	struct vcam *dev;
	int ret;

	dev = video_drvdata(file);
	ret = _vb2_fop_release(file, NULL);
	clear_bit(VCAM_FLAG_IS_OPEN, &dev->flags);

	kref_put(&dev->ref, vcam_release);
	return ret;
}

static const struct v4l2_file_operations vcam_vdev_fops = {
	.owner = THIS_MODULE,
	.open = vcam_vdev_open,
	.release = vcam_vdev_close,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int vcam_ioc_create_validate(struct vcam_ioc_create *config,
				    char *card_label)
{
	long len, i;

	if (config->device_nr != 0)
		return -EINVAL;
	if (config->reserved)
		return -EINVAL;
	if (config->nr_frames > VCAM_MAX_FRAMES)
		return -E2BIG;
	if (config->nr_frames < VCAM_MIN_FRAMES)
		return -EINVAL;
	if (!config->frames)
		return -EINVAL;

	memset(card_label, 0, VCAM_CARD_LABEL_MAX);
	len = strncpy_from_user(card_label,
				u64_to_user_ptr(config->device_name),
				VCAM_CARD_LABEL_MAX);
	if (len < 0)
		return -EFAULT;
	if (len >= VCAM_CARD_LABEL_MAX)
		return -E2BIG;
	if (!len)
		return -EINVAL;
	if (!isalnum((unsigned char)card_label[0]))
		return -EINVAL;
	for (i = 0; i < len; i++) {
		if (!isalnum((unsigned char)card_label[i]) &&
		    !isspace((unsigned char)card_label[i]))
			return -EINVAL;
	}
	if (!isalnum((unsigned char)card_label[len - 1]))
		return -EINVAL;

	return len;
}

static int vcam_vb2_queue_setup(struct vb2_queue *queue,
				unsigned int *nr_buffers,
				unsigned int *nr_planes, unsigned int sizes[],
				struct device *alloc_devs[])
{
	struct vcam *data = vb2_get_drv_priv(queue);
	unsigned int sizeimage = data->pix_format.sizeimage;

	if (!sizeimage)
		return -EINVAL;

	if (*nr_buffers < VCAM_MIN_FRAMES)
		*nr_buffers = VCAM_MIN_FRAMES;

	if (*nr_planes)
		return sizes[0] < sizeimage ? -EINVAL : 0;

	*nr_planes = 1;
	sizes[0] = sizeimage;
	return 0;
}

static int vcam_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct vcam *data = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vcam_buf *buf = container_of(vbuf, struct vcam_buf, vb);
	unsigned int sizeimage = data->pix_format.sizeimage;
	unsigned int bytesused;
	void *vaddr;

	if (vb2_plane_size(vb, 0) < sizeimage)
		return -EINVAL;

	vbuf->field = data->pix_format.field;
	bytesused = vb2_get_plane_payload(vb, 0);
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) && !bytesused)
		vb2_set_plane_payload(vb, 0, sizeimage);

	buf->flags = VCAM_BUF_FLAG_MAPPABLE;
	if (vb->planes[0].dbuf) {
		vaddr = vb2_plane_vaddr(vb, 0);
		if (!vaddr)
			buf->flags &= ~VCAM_BUF_FLAG_MAPPABLE;
	}
	return 0;
}

static void vcam_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vcam *data = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vcam_buf *buf;
	unsigned long flags;

	buf = container_of(vbuf, struct vcam_buf, vb);

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		spin_lock_irqsave(&data->frame_lock, flags);
		list_add_tail(&buf->list, &data->output_list);
		spin_unlock_irqrestore(&data->frame_lock, flags);
	} else {
		spin_lock_irqsave(&data->frame_lock, flags);
		list_add_tail(&buf->list, &data->capture_list);
		spin_unlock_irqrestore(&data->frame_lock, flags);
	}

	vcam_dequeue_frames(data);
}

static int vcam_vb2_prepare_streaming(struct vb2_queue *vq)
{
	return 0;
}

static int vcam_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vcam *data = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_CAPTURE(vq->type)) {
		atomic_set(&data->sequence, 0);
		vcam_status_update_stream(data, true);
	}

	vcam_dequeue_frames(data);
	return 0;
}

static void vcam_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct vcam *data = vb2_get_drv_priv(vq);
	struct vcam_buf *buf, *tmp;
	unsigned long flags;
	LIST_HEAD(done_list);

	if (V4L2_TYPE_IS_CAPTURE(vq->type)) {
		vcam_status_update_stream(data, false);
		spin_lock_irqsave(&data->frame_lock, flags);
		list_splice_init(&data->capture_list, &done_list);
		list_splice_init(&data->output_list, &done_list);
		spin_unlock_irqrestore(&data->frame_lock, flags);

		list_for_each_entry_safe(buf, tmp, &done_list, list)
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);

		return;
	}

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		spin_lock_irqsave(&data->frame_lock, flags);
		list_splice_init(&data->output_list, &done_list);
		list_splice_init(&data->capture_list, &done_list);
		spin_unlock_irqrestore(&data->frame_lock, flags);

		list_for_each_entry_safe(buf, tmp, &done_list, list)
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops vcam_vb2_ops = {
	.queue_setup = vcam_vb2_queue_setup,
	.buf_queue = vcam_vb2_buf_queue,
	.buf_prepare = vcam_vb2_buf_prepare,
	.prepare_streaming = vcam_vb2_prepare_streaming,
	.start_streaming = vcam_vb2_start_streaming,
	.stop_streaming = vcam_vb2_stop_streaming,
};

static int vcam_ioc_create(struct file *file, struct vcam *dev,
			   struct vcam_ioc_create *config, char *card_label,
			   unsigned int len)
{
	struct v4l2_format try_fmt;
	struct video_device *vdev;
	struct vb2_queue *queue;
	struct v4l2_format fmt;
	long ret;

	strscpy(dev->v4l2_dev.name, "vcam", sizeof(dev->v4l2_dev.name));

	ret = v4l2_device_register(NULL, &dev->v4l2_dev);
	if (ret)
		return ret;

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_unregister;
	}

	dev->vdev = vdev;
	video_set_drvdata(vdev, dev);
	memcpy(vdev->name, card_label, len);
	vdev->name[len] = '\0';
	vdev->vfl_type = VFL_TYPE_VIDEO;
	vdev->fops = &vcam_vdev_fops;
	vdev->ioctl_ops = &vcam_ioctl_ops;
	vdev->release = &video_device_release;
	vdev->minor = -1;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->vfl_dir = VFL_DIR_RX;

	mutex_init(&dev->lock);
	spin_lock_init(&dev->frame_lock);
	spin_lock_init(&dev->status_lock);
	INIT_LIST_HEAD(&dev->capture_list);
	INIT_LIST_HEAD(&dev->output_list);
	dev->status = VCAM_STATUS_IDLE;
	dev->output_memory = VB2_MEMORY_DMABUF;
	init_waitqueue_head(&dev->status_waitq);

	dev->vdev->v4l2_dev = &dev->v4l2_dev;
	dev->vdev->queue = &dev->capture_queue;
	dev->vdev->lock = &dev->lock;
	dev->capture.capability = 0;
	dev->capture.capturemode = 0;
	dev->capture.extendedmode = 0;
	dev->capture.readbuffers = VCAM_MIN_FRAMES;
	dev->capture.timeperframe.numerator = 1;
	dev->capture.timeperframe.denominator = 30;

	if (!IS_ENABLED(CONFIG_DMA_SHARED_BUFFER) ||
	    !vb2_vmalloc_memops.attach_dmabuf) {
		ret = -EOPNOTSUPP;
		goto err_unregister;
	}

	fmt = (struct v4l2_format){
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.fmt.pix = { .width = config->width,
			     .height = config->height,
			     .pixelformat = config->pixelformat,
			     .colorspace = config->colorspace,
			     .bytesperline = config->bytesperline,
			     .field = V4L2_FIELD_NONE }
	};

	try_fmt = fmt;

	ret = vcam_set_format(dev, &try_fmt);
	if (ret)
		goto err_unregister;

	if ((fmt.fmt.pix.width && try_fmt.fmt.pix.width != fmt.fmt.pix.width) ||
	    (fmt.fmt.pix.height &&
	     try_fmt.fmt.pix.height != fmt.fmt.pix.height) ||
	    try_fmt.fmt.pix.pixelformat != fmt.fmt.pix.pixelformat ||
	    (fmt.fmt.pix.colorspace != V4L2_COLORSPACE_DEFAULT &&
	     try_fmt.fmt.pix.colorspace != fmt.fmt.pix.colorspace) ||
	    (fmt.fmt.pix.bytesperline &&
	     try_fmt.fmt.pix.bytesperline != fmt.fmt.pix.bytesperline)) {
		ret = -EINVAL;
		goto err_unregister;
	}

	dev->pix_format = try_fmt.fmt.pix;

	queue = &dev->capture_queue;
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->drv_priv = dev;
	queue->buf_struct_size = sizeof(struct vcam_buf);
	queue->ops = &vcam_vb2_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->lock = &dev->lock;
	queue->dev = &dev->vdev->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto err_unregister;

	queue = &dev->output_queue;
	queue->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	queue->io_modes = VB2_DMABUF;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	queue->drv_priv = dev;
	queue->buf_struct_size = sizeof(struct vcam_buf);
	queue->ops = &vcam_vb2_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->lock = &dev->lock;
	queue->dev = &dev->vdev->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto err_capture_queue;

	ret = vcam_ioc_alloc(file, dev, config->nr_frames,
			     u64_to_user_ptr(config->frames),
			     VB2_MEMORY_DMABUF);
	if (ret)
		goto err_output_queue;

	ret = video_register_device(dev->vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0)
		goto err_output_queue;

	config->device_nr = dev->vdev->num;
	return 0;

err_output_queue:
	vb2_queue_release(&dev->output_queue);

err_capture_queue:
	vb2_queue_release(&dev->capture_queue);

err_unregister:
	if (dev->vdev)
		video_device_release(dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);
	return ret;
}

static long vcam_ioctl(struct file *file, unsigned int cmd, unsigned long parm)
{
	struct vcam *dev = file->private_data;
	char card_label[VCAM_CARD_LABEL_MAX];
	struct vcam_ioc_create config;
	long ret, len;

	if (cmd != VCAM_IOC_CREATE) {
		if (!dev || !test_bit(VCAM_FLAG_CREATING, &dev->flags) ||
		    !test_bit(VCAM_FLAG_READY, &dev->flags) ||
		    dev->device_nr < 0)
			return -ENOTTY;
		return vcam_ioctl_common(file, cmd, parm);
	}

	if (!dev)
		return -ENOTTY;

	if (test_and_set_bit(VCAM_FLAG_CREATING, &dev->flags))
		return -EBUSY;

	if (!parm) {
		ret = -EINVAL;
		goto err_clear;
	}

	if (copy_from_user(&config, (void *)parm, sizeof(config))) {
		ret = -EFAULT;
		goto err_clear;
	}

	len = vcam_ioc_create_validate(&config, card_label);
	if (len < 0) {
		ret = len;
		goto err_clear;
	}

	ret = vcam_ioc_create(file, dev, &config, card_label, len);
	if (ret)
		goto err_clear;

	if (copy_to_user((void *)parm, &config, sizeof(config))) {
		ret = -EFAULT;
		goto err_release;
	}

	dev->device_nr = dev->vdev->num;
	snprintf(dev->v4l2_dev.name, sizeof(dev->v4l2_dev.name), "vcam-%d",
		 dev->device_nr);
	set_bit(VCAM_FLAG_READY, &dev->flags);
	return 0;

err_release:
	__vcam_release(dev);

err_clear:
	clear_bit(VCAM_FLAG_CREATING, &dev->flags);
	return ret;
}

static const struct file_operations vcam_fops = {
	.owner = THIS_MODULE,
	.open = vcam_open,
	.unlocked_ioctl = vcam_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vcam_ioctl,
#endif
	.poll = vcam_poll,
	.mmap = vcam_mmap,
	.release = vcam_close,
	.llseek = noop_llseek,
};

static struct miscdevice vcam_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vcam",
	.fops = &vcam_fops,
	.groups = vcam_attr_groups,
};

module_misc_device(vcam_misc);
