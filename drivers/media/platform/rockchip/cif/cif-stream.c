// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>

#include "cif-common.h"
#include "cif-stream.h"

#define CIF_REQ_BUFS_MIN 8
#define CIF_MIN_WIDTH 64
#define CIF_MIN_HEIGHT 64
#define CIF_MAX_WIDTH 8192
#define CIF_MAX_HEIGHT 8192

static inline struct cif_buffer *to_cif_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct cif_buffer, vb);
}

static inline struct cif_stream *to_cif_stream(struct video_device *vdev)
{
	return container_of(vdev, struct cif_stream, vdev);
}

static const struct cif_output_fmt *
cif_stream_find_output_fmt(struct cif_stream *stream, u32 pixelfmt)
{
	const struct cif_output_fmt *fmt;
	unsigned int i;

	for (i = 0; i < stream->out_fmts_num; i++) {
		fmt = &stream->out_fmts[i];
		if (fmt->fourcc == pixelfmt)
			return fmt;
	}

	return NULL;
}

static struct cif_buffer *cif_stream_pop_buffer(struct cif_stream *stream)
{
	struct cif_buffer *buffer = NULL;
	unsigned long lock_flags;

	spin_lock_irqsave(&stream->driver_queue_lock, lock_flags);

	if (list_empty(&stream->driver_queue))
		goto err_empty;

	buffer = list_first_entry(&stream->driver_queue, struct cif_buffer,
				  queue);
	list_del(&buffer->queue);

err_empty:
	spin_unlock_irqrestore(&stream->driver_queue_lock, lock_flags);
	return buffer;
}

static void cif_stream_push_buffer(struct cif_stream *stream,
				   struct cif_buffer *buffer)
{
	unsigned long lock_flags;

	spin_lock_irqsave(&stream->driver_queue_lock, lock_flags);
	list_add_tail(&buffer->queue, &stream->driver_queue);
	spin_unlock_irqrestore(&stream->driver_queue_lock, lock_flags);
}

static inline void cif_stream_return_buffer(struct cif_stream *stream,
					    struct cif_buffer *buffer,
					    enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *vb = &buffer->vb;

	vb2_buffer_done(&vb->vb2_buf, state);
}

static void cif_stream_complete_buffer(struct cif_stream *stream,
				       struct cif_buffer *buffer)
{
	struct vb2_v4l2_buffer *vb = &buffer->vb;

	vb->vb2_buf.timestamp = ktime_get_ns();
	vb->sequence = stream->frame_idx;
	vb2_buffer_done(&vb->vb2_buf, VB2_BUF_STATE_DONE);
	stream->frame_idx++;
}

void cif_stream_pingpong(struct cif_stream *stream)
{
	struct cif_buffer *buffer;

	if (!stream->buffers[stream->frame_phase]->is_dummy)
		cif_stream_complete_buffer(
			stream, stream->buffers[stream->frame_phase]);

	buffer = cif_stream_pop_buffer(stream);
	if (buffer) {
		stream->buffers[stream->frame_phase] = buffer;
		stream->buffers[stream->frame_phase]->is_dummy = false;
	} else {
		stream->buffers[stream->frame_phase] = &stream->dummy.buffer;
		stream->buffers[stream->frame_phase]->is_dummy = true;
		dev_warn(stream->cif_dev->dev,
			 "no buffer available, frame will be dropped\n");
	}

	if (stream->queue_buffer)
		stream->queue_buffer(stream, stream->frame_phase);

	stream->frame_phase = 1 - stream->frame_phase;
}

static int cif_stream_init_buffers(struct cif_stream *stream)
{
	const struct cif_output_fmt *fmt = stream->active_out_fmt;
	struct v4l2_pix_format_mplane *pix = &stream->pix;
	int i;

	stream->buffers[0] = cif_stream_pop_buffer(stream);
	if (!stream->buffers[0])
		goto err_buff_0;

	stream->buffers[1] = cif_stream_pop_buffer(stream);
	if (!stream->buffers[1])
		goto err_buff_1;

	if (stream->queue_buffer) {
		stream->queue_buffer(stream, 0);
		stream->queue_buffer(stream, 1);
	}

	stream->dummy.size = fmt->cplanes * pix->plane_fmt[0].sizeimage;
	stream->dummy.vaddr =
		dma_alloc_attrs(stream->cif_dev->dev, stream->dummy.size,
				&stream->dummy.buffer.buff_addr[0], GFP_KERNEL,
				DMA_ATTR_NO_KERNEL_MAPPING);
	if (!stream->dummy.vaddr)
		goto err_dummy;

	for (i = 1; i < fmt->cplanes; i++)
		stream->dummy.buffer.buff_addr[i] =
			stream->dummy.buffer.buff_addr[i - 1] +
			pix->plane_fmt[i - 1].bytesperline * pix->height;

	return 0;

err_dummy:
	cif_stream_return_buffer(stream, stream->buffers[1],
				 VB2_BUF_STATE_QUEUED);
	stream->buffers[1] = NULL;

err_buff_1:
	cif_stream_return_buffer(stream, stream->buffers[0],
				 VB2_BUF_STATE_QUEUED);
	stream->buffers[0] = NULL;
err_buff_0:
	return -EINVAL;
}

static void cif_stream_return_all_buffers(struct cif_stream *stream,
					  enum vb2_buffer_state state)
{
	struct cif_buffer *buffer;

	dma_free_attrs(stream->cif_dev->dev, stream->dummy.size,
		       stream->dummy.vaddr, stream->dummy.buffer.buff_addr[0],
		       DMA_ATTR_NO_KERNEL_MAPPING);

	if (stream->buffers[0]) {
		cif_stream_return_buffer(stream, stream->buffers[0], state);
		stream->buffers[0] = NULL;
	}

	if (stream->buffers[1]) {
		cif_stream_return_buffer(stream, stream->buffers[1], state);
		stream->buffers[1] = NULL;
	}

	while ((buffer = cif_stream_pop_buffer(stream)))
		cif_stream_return_buffer(stream, buffer, state);
}

static int cif_stream_setup_queue(struct vb2_queue *queue,
				  unsigned int *num_buffers,
				  unsigned int *num_planes,
				  unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct cif_stream *stream = queue->drv_priv;
	struct v4l2_pix_format_mplane *pix = &stream->pix;
	unsigned int i;

	if (*num_planes) {
		if (*num_planes != pix->num_planes)
			return -EINVAL;

		for (i = 0; i < pix->num_planes; i++)
			if (sizes[i] < pix->plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*num_planes = pix->num_planes;
		for (i = 0; i < pix->num_planes; i++)
			sizes[i] = pix->plane_fmt[i].sizeimage;
	}

	return 0;
}

static int cif_stream_prepare_buffer(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct cif_buffer *buffer = to_cif_buffer(vbuf);
	struct cif_stream *stream = vb->vb2_queue->drv_priv;
	const struct cif_output_fmt *fmt = stream->active_out_fmt;
	struct v4l2_pix_format_mplane *pix = &stream->pix;
	unsigned int i;

	memset(buffer->buff_addr, 0, sizeof(buffer->buff_addr));
	for (i = 0; i < pix->num_planes; i++)
		buffer->buff_addr[i] = vb2_dma_contig_plane_dma_addr(vb, i);

	/* apply fallback for non-mplane formats, if required */
	if (pix->num_planes == 1) {
		for (i = 1; i < fmt->cplanes; i++)
			buffer->buff_addr[i] =
				buffer->buff_addr[i - 1] +
				pix->plane_fmt[i - 1].bytesperline *
					pix->height;
	}

	for (i = 0; i < pix->num_planes; i++) {
		unsigned long size = pix->plane_fmt[i].sizeimage;

		if (vb2_plane_size(vb, i) < size) {
			dev_err(stream->cif_dev->dev,
				"user buffer too small (%ld < %ld)\n",
				vb2_plane_size(vb, i), size);
			return -EINVAL;
		}

		vb2_set_plane_payload(vb, i, size);
	}

	return 0;
}

static void cif_stream_queue_buffer(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct cif_buffer *buffer = to_cif_buffer(vbuf);
	struct cif_stream *stream = vb->vb2_queue->drv_priv;

	cif_stream_push_buffer(stream, buffer);
}

static int cif_stream_start_streaming(struct vb2_queue *queue,
				      unsigned int count)
{
	struct cif_stream *stream = queue->drv_priv;
	struct cif_device *cif_dev = stream->cif_dev;
	struct v4l2_subdev *sd = stream->remote->sd;
	int ret;

	stream->frame_idx = 0;
	stream->frame_phase = 0;

	ret = video_device_pipeline_start(&stream->vdev, &stream->pipeline);
	if (ret) {
		dev_err(cif_dev->dev, "failed to start pipeline %d\n", ret);
		goto err_out;
	}

	ret = pm_runtime_resume_and_get(cif_dev->dev);
	if (ret < 0) {
		dev_err(cif_dev->dev, "failed to get runtime pm, %d\n", ret);
		goto err_pipeline_stop;
	}

	ret = cif_stream_init_buffers(stream);
	if (ret)
		goto err_runtime_put;

	if (stream->start_streaming) {
		ret = stream->start_streaming(stream);
		if (ret < 0)
			goto err_runtime_put;
	}

	ret = v4l2_subdev_call(sd, video, s_stream, 1);
	if (ret < 0)
		goto err_stop_stream;

	return 0;

err_stop_stream:
	if (stream->stop_streaming)
		stream->stop_streaming(stream);
err_runtime_put:
	pm_runtime_put(cif_dev->dev);
err_pipeline_stop:
	video_device_pipeline_stop(&stream->vdev);
err_out:
	cif_stream_return_all_buffers(stream, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void cif_stream_stop_streaming(struct vb2_queue *queue)
{
	struct cif_stream *stream = queue->drv_priv;
	struct cif_device *cif_dev = stream->cif_dev;
	struct v4l2_subdev *sd = stream->remote->sd;
	int ret;

	v4l2_subdev_call(sd, video, s_stream, 0);

	stream->stopping = true;
	ret = wait_event_timeout(stream->wq_stopped, !stream->stopping,
				 msecs_to_jiffies(1000));

	if (!ret && stream->stop_streaming)
		stream->stop_streaming(stream);

	pm_runtime_put(cif_dev->dev);

	cif_stream_return_all_buffers(stream, VB2_BUF_STATE_ERROR);

	video_device_pipeline_stop(&stream->vdev);
}

static const struct vb2_ops cif_stream_vb2_ops = {
	.queue_setup = cif_stream_setup_queue,
	.buf_prepare = cif_stream_prepare_buffer,
	.buf_queue = cif_stream_queue_buffer,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = cif_stream_start_streaming,
	.stop_streaming = cif_stream_stop_streaming,
};

static int cif_stream_try_format(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct cif_stream *stream = video_drvdata(file);
	const struct cif_output_fmt *fmt;
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	struct v4l2_subdev *sd = stream->remote->sd;
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	u32 height, width;
	int ret;

	fmt = cif_stream_find_output_fmt(stream, pix->pixelformat);
	if (!fmt)
		fmt = &stream->out_fmts[0];

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret < 0)
		return ret;

	height = clamp_t(u32, sd_fmt.format.height, CIF_MIN_HEIGHT,
			 CIF_MAX_HEIGHT);
	width = clamp_t(u32, sd_fmt.format.width, CIF_MIN_WIDTH, CIF_MAX_WIDTH);

	pix->field = sd_fmt.format.field;
	pix->colorspace = sd_fmt.format.colorspace;
	pix->ycbcr_enc = sd_fmt.format.ycbcr_enc;
	pix->quantization = sd_fmt.format.quantization;
	pix->xfer_func = sd_fmt.format.xfer_func;

	v4l2_fill_pixfmt_mp(pix, fmt->fourcc, width, height);

	return 0;
}

static int cif_stream_set_format(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct cif_stream *stream = video_drvdata(file);
	int ret;

	if (vb2_is_busy(&stream->buf_queue))
		return -EBUSY;

	ret = cif_stream_try_format(file, priv, f);
	if (ret)
		return ret;

	stream->pix = f->fmt.pix_mp;
	stream->active_out_fmt =
		cif_stream_find_output_fmt(stream, f->fmt.pix_mp.pixelformat);

	return 0;
}

static int cif_stream_get_format(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct cif_stream *stream = video_drvdata(file);

	f->fmt.pix_mp = stream->pix;

	return 0;
}

static int cif_stream_enum_formats(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	struct cif_stream *stream = video_drvdata(file);

	if (f->index >= stream->out_fmts_num)
		return -EINVAL;

	f->pixelformat = stream->out_fmts[f->index].fourcc;

	return 0;
}

static int cif_stream_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *fsize)
{
	struct cif_stream *stream = video_drvdata(file);
	struct v4l2_subdev_frame_size_enum fse = {
		.index = fsize->index,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	struct v4l2_subdev *sd = stream->remote->sd;
	const struct cif_output_fmt *fmt;
	int ret;

	fmt = cif_stream_find_output_fmt(stream, fsize->pixel_format);
	if (!fmt)
		return -EINVAL;

	fse.code = fmt->mbus_code;

	ret = v4l2_subdev_call(sd, pad, enum_frame_size, NULL, &fse);
	if (ret)
		return ret;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = fse.max_width;
	fsize->discrete.height = fse.max_height;

	/* some variants may have a scaler in the path. */
	/* TODO: add support for them */

	return 0;
}

static int cif_stream_enum_frameintervals(struct file *file, void *fh,
					  struct v4l2_frmivalenum *fival)
{
	struct cif_stream *stream = video_drvdata(file);
	struct v4l2_subdev *sd = stream->remote->sd;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index = fival->index,
		.width = fival->width,
		.height = fival->height,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	const struct cif_output_fmt *fmt;
	int ret;

	fmt = cif_stream_find_output_fmt(stream, fival->pixel_format);
	if (!fmt)
		return -EINVAL;

	fie.code = fmt->mbus_code;

	ret = v4l2_subdev_call(sd, pad, enum_frame_interval, NULL, &fie);
	if (ret)
		return ret;

	fival->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fival->discrete = fie.interval;

	return 0;
}

static int cif_stream_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct cif_stream *stream = video_drvdata(file);
	struct device *dev = stream->cif_dev->dev;

	strscpy(cap->driver, dev->driver->name, sizeof(cap->driver));
	strscpy(cap->card, dev->driver->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(dev));

	return 0;
}

static const struct v4l2_ioctl_ops cif_stream_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_try_fmt_vid_cap_mplane = cif_stream_try_format,
	.vidioc_s_fmt_vid_cap_mplane = cif_stream_set_format,
	.vidioc_g_fmt_vid_cap_mplane = cif_stream_get_format,
	.vidioc_enum_fmt_vid_cap = cif_stream_enum_formats,
	.vidioc_enum_framesizes = cif_stream_enum_framesizes,
	.vidioc_enum_frameintervals = cif_stream_enum_frameintervals,
	.vidioc_querycap = cif_stream_querycap,
	.vidioc_subscribe_event = v4l2_src_change_event_subscribe,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int cif_stream_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		media_entity_to_video_device(link->sink->entity);
	struct v4l2_subdev *sd =
		media_entity_to_v4l2_subdev(link->source->entity);
	struct cif_stream *stream = to_cif_stream(vdev);
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
	};
	const struct cif_input_fmt *cif_in_fmt = NULL;
	unsigned int i;
	int ret;

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret < 0)
		goto err_out;

	for (i = 0; i < stream->in_fmts_num; i++) {
		if (sd_fmt.format.code == stream->in_fmts[i].mbus_code &&
		    sd_fmt.format.field == stream->in_fmts[i].field) {
			cif_in_fmt = &stream->in_fmts[i];
			break;
		}
	}
	if (!cif_in_fmt) {
		dev_err(stream->cif_dev->dev,
			"remote's mbus code not supported\n");
		goto err_out;
	}

	if (sd_fmt.format.height != stream->pix.height ||
	    sd_fmt.format.width != stream->pix.width) {
		dev_err(stream->cif_dev->dev,
			"link '%s':%u -> '%s':%u not valid: %ux%u != %ux%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.width, sd_fmt.format.height,
			stream->pix.width, stream->pix.height);
		goto err_out;
	}

	stream->active_in_fmt = cif_in_fmt;
	return 0;

err_out:
	stream->active_in_fmt = NULL;
	return -EPIPE;
}

static const struct media_entity_operations cif_stream_media_ops = {
	.link_validate = cif_stream_link_validate,
};

static const struct v4l2_file_operations cif_stream_file_ops = {
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int cif_stream_init_vb2_queue(struct vb2_queue *q,
				     struct cif_stream *stream)
{
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = stream;
	q->ops = &cif_stream_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->buf_struct_size = sizeof(struct cif_buffer);
	q->min_queued_buffers = CIF_REQ_BUFS_MIN;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &stream->vlock;
	q->dev = stream->cif_dev->dev;

	return vb2_queue_init(q);
}

int cif_stream_register(struct cif_device *cif_dev, struct cif_stream *stream,
			const struct cif_stream_config *config)
{
	struct v4l2_device *v4l2_dev = &cif_dev->v4l2_dev;
	struct video_device *vdev = &stream->vdev;
	int ret;

	stream->cif_dev = cif_dev;

	INIT_LIST_HEAD(&stream->driver_queue);
	spin_lock_init(&stream->driver_queue_lock);

	init_waitqueue_head(&stream->wq_stopped);

	mutex_init(&stream->vlock);

	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->entity.ops = &cif_stream_media_ops;
	vdev->fops = &cif_stream_file_ops;
	vdev->ioctl_ops = &cif_stream_ioctl_ops;
	vdev->lock = &stream->vlock;
	vdev->minor = -1;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	video_set_drvdata(vdev, stream);

	stream->pad.flags = MEDIA_PAD_FL_SINK;

	cif_stream_init_vb2_queue(&stream->buf_queue, stream);

	vdev->queue = &stream->buf_queue;
	if (config->name)
		strscpy(vdev->name, config->name, sizeof(vdev->name));

	ret = media_entity_pads_init(&vdev->entity, 1, &stream->pad);
	if (ret < 0) {
		dev_err(cif_dev->dev, "failed to initialize media pads: %d\n",
			ret);
		return ret;
	}

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_err(cif_dev->dev, "failed to register video device: %d\n",
			ret);
		goto err_media_entity_cleanup;
	}

	v4l2_info(v4l2_dev, "registered %s as /dev/video%d\n", vdev->name,
		  vdev->num);

	return 0;

err_media_entity_cleanup:
	media_entity_cleanup(&stream->vdev.entity);
	return ret;
}

void cif_stream_unregister(struct cif_stream *stream)
{
	video_unregister_device(&stream->vdev);
	media_entity_cleanup(&stream->vdev.entity);
}
