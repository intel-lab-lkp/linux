// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/align.h>
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>

#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c720-common.h"

static const char * const capture_device_names[] = {
	"mali-c720 fr0-rgb",
	"mali-c720 fr0-yuv",
};

static int mali_c720_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		media_entity_to_video_device(link->sink->entity);
	struct mali_c720_cap_dev *cap_dev = video_get_drvdata(vdev);
	struct v4l2_subdev *sd =
		media_entity_to_v4l2_subdev(link->source->entity);
	const struct v4l2_pix_format_mplane *pix_mp;
	const struct mali_c720_v4l2_format_info *cap_fmt;
	int ret;

	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret)
		return ret;

	pix_mp = &cap_dev->format.pix_mp;
	cap_fmt = cap_dev->format.info;

	if (sd_fmt.format.width != pix_mp->width ||
	    sd_fmt.format.height != pix_mp->height) {
		dev_err(cap_dev->mali_c720->dev,
			"link '%s':%u -> '%s':%u not valid: %ux%u != %ux%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.width, sd_fmt.format.height,
			pix_mp->width, pix_mp->height);
		return -EPIPE;
	}

	if (!mali_c720_mbus_code_supports_fmt(cap_fmt, sd_fmt.format.code)) {
		dev_err(cap_dev->mali_c720->dev,
			"link '%s':%u -> '%s':%u not valid: mbus_code 0x%04x cannot produce pixel format %p4cc\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.code, &pix_mp->pixelformat);
		return -EPIPE;
	}

	return 0;
}

static const struct media_entity_operations mali_c720_media_ops = {
	.link_validate = mali_c720_link_validate,
};

static int mali_c720_vb2_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				     unsigned int *num_planes, unsigned int sizes[],
				     struct device *alloc_devs[])
{
	struct mali_c720_cap_dev *cap_dev = q->drv_priv;
	unsigned int i;

	if (*num_planes) {
		if (*num_planes != cap_dev->format.pix_mp.num_planes)
			return -EINVAL;

		for (i = 0; i < cap_dev->format.pix_mp.num_planes; i++)
			if (sizes[i] < cap_dev->format.pix_mp.plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*num_planes = cap_dev->format.pix_mp.num_planes;
		for (i = 0; i < cap_dev->format.pix_mp.num_planes; i++)
			sizes[i] = cap_dev->format.pix_mp.plane_fmt[i].sizeimage;
	}

	return 0;
}

static void mali_c720_buf_queue(struct vb2_buffer *vb)
{
	struct mali_c720_cap_dev *cap_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_cap_buffer *buf = container_of(vbuf,
						   struct mali_c720_cap_buffer, vb);
	unsigned int i;

	for (i = 0; i < cap_dev->format.pix_mp.num_planes; i++) {
		unsigned long size = cap_dev->format.pix_mp.plane_fmt[i].sizeimage;

		vb2_set_plane_payload(vb, i, size);
	}

	buf->vb.field = V4L2_FIELD_NONE;

	scoped_guard(spinlock, &cap_dev->buffers.lock)
		list_add_tail(&buf->queue, &cap_dev->buffers.input);

	if (!mali_c720_cap_dev_uses_requests(cap_dev))
		mali_c720_try_schedule_frame(ctx);
}

static int mali_c720_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_cap_buffer *buf = container_of(vbuf,
						   struct mali_c720_cap_buffer, vb);

	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (!IS_ALIGNED(buf->addr, 32)) {
		dev_dbg(vb->vb2_queue->dev,
			"Capture buffer DMA address is not 32-byte aligned\n");
		return -EINVAL;
	}

	return 0;
}

static void mali_c720_buf_request_complete(struct vb2_buffer *vb)
{
}

bool mali_c720_cap_dev_uses_requests(struct mali_c720_cap_dev *cap_dev)
{
	return video_is_registered(&cap_dev->vdev) &&
	       vb2_is_streaming(&cap_dev->queue) &&
	       cap_dev->queue.uses_requests;
}

struct media_request *mali_c720_cap_dev_get_request(struct mali_c720_cap_dev *cap_dev)
{
	struct mali_c720_cap_buffer *buf;

	if (!mali_c720_cap_dev_uses_requests(cap_dev))
		return NULL;

	guard(spinlock)(&cap_dev->buffers.lock);

	buf = list_first_entry_or_null(&cap_dev->buffers.input,
				       struct mali_c720_cap_buffer, queue);

	return buf ? buf->vb.vb2_buf.request : NULL;
}

int mali_c720_cap_dev_is_ready(struct mali_c720_cap_dev *cap_dev)
{
	struct mali_c720_cap_buffer *buf;

	if (!video_is_registered(&cap_dev->vdev))
		return -ENODEV;

	if (!vb2_is_streaming(&cap_dev->queue))
		return -ENODEV;

	scoped_guard(spinlock, &cap_dev->buffers.lock) {
		buf = list_first_entry_or_null(&cap_dev->buffers.input,
					       struct mali_c720_cap_buffer, queue);

		if (!buf)
			return -ENOENT;
	}

	return 0;
}

void mali_c720_cap_dev_set_next_buffer(struct mali_c720_cap_dev *cap_dev,
				       struct media_request *req)
{
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_buffer_pool *pool = &cap_dev->buffers.pool;
	struct mali_c720_cap_buffer *buf;

	if (!video_is_registered(&cap_dev->vdev))
		return;

	if (!vb2_is_streaming(&cap_dev->queue))
		return;

	scoped_guard(spinlock, &cap_dev->buffers.lock) {
		buf = list_first_entry_or_null(&cap_dev->buffers.input,
					       struct mali_c720_cap_buffer, queue);

		if (buf && (!buf->vb.vb2_buf.request ||
			    buf->vb.vb2_buf.request == req)) {
			mali_c720_ctx_mgr_update_buffer(ctx, pool, &buf->addr, 0);
			list_move_tail(&buf->queue, &cap_dev->buffers.processing);
			return;
		}
	}

	mali_c720_ctx_mgr_update_buffer(ctx, pool, NULL, 0);
}

void mali_c720_cap_dev_buffer_done(struct mali_c720_cap_dev *cap_dev,
				   bool success)
{
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_cap_buffer *buf;

	if (!video_is_registered(&cap_dev->vdev))
		return;

	if (!vb2_is_streaming(&cap_dev->queue))
		return;

	guard(spinlock)(&cap_dev->buffers.lock);

	buf = list_first_entry_or_null(&cap_dev->buffers.processing,
				       struct mali_c720_cap_buffer, queue);
	if (!buf)
		return;

	if (mali_c720_ctx_mgr_check_output_buffer_status(ctx, &cap_dev->buffers.pool))
		success = false;

	if (success) {
		buf->vb.vb2_buf.timestamp = ctx->timestamp;
		buf->vb.sequence = ctx->frame_sequence;
	}

	vb2_buffer_done(&buf->vb.vb2_buf, success ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	list_del(&buf->queue);
}

static void mali_c720_cap_dev_return_buffers(struct mali_c720_cap_dev *cap_dev,
					     enum vb2_buffer_state state)
{
	struct mali_c720_cap_buffer *buf, *tmp;

	guard(spinlock)(&cap_dev->buffers.lock);

	list_for_each_entry_safe(buf, tmp, &cap_dev->buffers.input, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	list_for_each_entry_safe(buf, tmp, &cap_dev->buffers.processing, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static int mali_c720_cap_dev_buffer_pool_configure(struct mali_c720_cap_dev *cap_dev)
{
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	int ret;

	ret = mali_c720_ctx_mgr_request_buffer_pool(ctx,
						    &cap_dev->format.buffer_request,
						    &cap_dev->buffers.pool);
	if (ret)
		return ret;

	return 0;
}

static void mali_c720_cap_dev_release_buffer_pool(struct mali_c720_cap_dev *cap_dev)
{
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_buffer_pool *pool = &cap_dev->buffers.pool;

	mali_c720_ctx_mgr_release_buffer_pool(ctx, pool);
}

static int mali_c720_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mali_c720_cap_dev *cap_dev = q->drv_priv;
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct v4l2_subdev *isp_sd = &mali_c720->isp.mdev.sd;
	int ret;

	guard(mutex)(&mali_c720->device_lock);

	if (!media_devnode_is_registered(mali_c720->media_dev.devnode)) {
		ret = -ENODEV;
		goto err_return_buffers;
	}

	ret = pm_runtime_resume_and_get(mali_c720->dev);
	if (ret)
		goto err_return_buffers;

	ret = video_device_pipeline_start(&cap_dev->vdev, &mali_c720->pipe);
	if (ret) {
		dev_err(mali_c720->dev, "%s failed to start media pipeline\n",
			cap_dev->vdev.name);
		goto err_pm_put;
	}

	ret = mali_c720_alloc_context(ctx);
	if (ret)
		goto err_stop_video_pipeline;

	ret = mali_c720_cap_dev_buffer_pool_configure(cap_dev);
	if (ret) {
		dev_err(mali_c720->dev, "%s failed to request output buffers\n",
			cap_dev->vdev.name);
		goto err_free_context;
	}

	ret = v4l2_subdev_enable_streams(cap_dev->src_sd, cap_dev->src_pad,
					 BIT(0));
	if (ret && ret != -EALREADY) {
		dev_err(mali_c720->dev, "%s failed to start src subdev\n",
			cap_dev->vdev.name);
		goto err_release_buffer_pool;
	}

	/*
	 * All ISP source pads use the same start process, so the HV pad can
	 * represent the ISP when starting the pipeline.
	 */
	ret = mali_c720_pipeline_ready(mali_c720);
	if (ret && ret != -EAGAIN)
		goto err_disable_src;

	if (!ret) {
		ret = v4l2_subdev_enable_streams(isp_sd,
						 MALI_C720_ISP_PAD_SOURCE_HV,
						 BIT(0));
		if (ret) {
			dev_err(mali_c720->dev, "%s failed to start ISP subdevice\n",
				cap_dev->vdev.name);
			goto err_disable_src;
		}
	}

	return 0;

err_disable_src:
	v4l2_subdev_disable_streams(cap_dev->src_sd, cap_dev->src_pad,
				    BIT(0));
err_release_buffer_pool:
	mali_c720_cap_dev_release_buffer_pool(cap_dev);
err_free_context:
	mali_c720_free_context(ctx);
err_stop_video_pipeline:
	video_device_pipeline_stop(&cap_dev->vdev);
err_pm_put:
	pm_runtime_put(mali_c720->dev);
err_return_buffers:
	mali_c720_cap_dev_return_buffers(cap_dev, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void mali_c720_vb2_stop_streaming(struct vb2_queue *q)
{
	struct mali_c720_cap_dev *cap_dev = q->drv_priv;
	struct mali_c720 *mali_c720 = cap_dev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct v4l2_subdev *isp_sd = &isp->mdev.sd;

	guard(mutex)(&mali_c720->device_lock);

	v4l2_subdev_disable_streams(isp_sd, MALI_C720_ISP_PAD_SOURCE_HV,
				    BIT(0));
	v4l2_subdev_disable_streams(cap_dev->src_sd, cap_dev->src_pad,
				    BIT(0));
	mali_c720_cap_dev_release_buffer_pool(cap_dev);
	mali_c720_cap_dev_return_buffers(cap_dev, VB2_BUF_STATE_ERROR);

	mali_c720_free_context(ctx);

	video_device_pipeline_stop(&cap_dev->vdev);

	pm_runtime_put_autosuspend(mali_c720->dev);
	pm_runtime_mark_last_busy(mali_c720->dev);
}

static const struct vb2_ops mali_c720_vb2_ops = {
	.queue_setup		= &mali_c720_vb2_queue_setup,
	.buf_queue		= &mali_c720_buf_queue,
	.buf_init		= &mali_c720_buf_init,
	.start_streaming	= &mali_c720_vb2_start_streaming,
	.stop_streaming		= &mali_c720_vb2_stop_streaming,
	.buf_request_complete	= &mali_c720_buf_request_complete,
};

static const struct v4l2_file_operations mali_c720_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static void mali_c720_try_fmt(struct mali_c720_cap_dev *cap_dev,
			      struct v4l2_pix_format_mplane *pix_mp)
{
	const struct mali_c720_v4l2_format_info *info;
	const struct mali_c720_mbus_format_info *cfg;
	struct v4l2_plane_pix_format *plane, *y_plane;
	u32 requested_stride = pix_mp->plane_fmt[0].bytesperline;
	unsigned int i;

	info = mali_c720_get_v4l2_config_by_fourcc(pix_mp->pixelformat);
	if (!info || !mali_c720_format_supported_by_vdev(info, cap_dev->id))
		info = mali_c720_get_v4l2_config_by_fourcc(cap_dev->format.default_format);

	cfg = mali_c720_get_mbus_config_by_code(info->mbus_code);

	pix_mp->pixelformat = info->fourcc;
	v4l_bound_align_image(&pix_mp->width, MALI_C720_MIN_OUTPUT_WIDTH,
			      MALI_C720_MAX_WIDTH, 1,
			      &pix_mp->height, MALI_C720_MIN_OUTPUT_HEIGHT,
			      MALI_C720_MAX_HEIGHT, 1, 0);

	pix_mp->field = V4L2_FIELD_NONE;
	if (cfg->type & (MALI_C720_FORMAT_RGB | MALI_C720_FORMAT_YUV))
		pix_mp->colorspace = V4L2_COLORSPACE_SRGB;
	else
		pix_mp->colorspace = V4L2_COLORSPACE_RAW;

	pix_mp->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix_mp->colorspace);
	if (cfg->type & MALI_C720_FORMAT_YUV)
		pix_mp->ycbcr_enc = V4L2_YCBCR_ENC_709;
	else
		pix_mp->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix_mp->colorspace);

	if (pix_mp->colorspace == V4L2_COLORSPACE_RAW)
		pix_mp->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		pix_mp->quantization =
			V4L2_MAP_QUANTIZATION_DEFAULT(cfg->type & MALI_C720_FORMAT_RGB,
						      pix_mp->colorspace,
						      pix_mp->ycbcr_enc);

	/* Only contiguous multi-planar formats are supported. */
	pix_mp->num_planes = 1;
	memset(pix_mp->plane_fmt, 0, sizeof(pix_mp->plane_fmt));

	y_plane = &pix_mp->plane_fmt[0];
	y_plane->bytesperline = clamp(requested_stride,
				      DIV_ROUND_UP(info->bpp[0] * pix_mp->width, 8),
				      65504U);
	y_plane->bytesperline = ALIGN(y_plane->bytesperline, 32);
	y_plane->sizeimage = y_plane->bytesperline * pix_mp->height;

	for (i = 1; i < info->comp_planes; i++) {
		plane = &pix_mp->plane_fmt[i];

		plane->bytesperline = y_plane->bytesperline;
		plane->sizeimage = DIV_ROUND_UP(plane->bytesperline * pix_mp->height,
						info->vdiv);

		y_plane->sizeimage += plane->sizeimage;
	}
}

static int mali_c720_try_fmt_vid_cap_mplane(struct file *file, void *fh,
					    struct v4l2_format *f)
{
	struct mali_c720_cap_dev *cap_dev = video_drvdata(file);

	mali_c720_try_fmt(cap_dev, &f->fmt.pix_mp);
	return 0;
}

static void mali_c720_set_format(struct mali_c720_cap_dev *cap_dev,
				 struct v4l2_pix_format_mplane *pix_mp)
{
	struct mali_c720_buffer_request_info *req = &cap_dev->format.buffer_request;
	const struct mali_c720_v4l2_format_info *info;
	unsigned int i;

	mali_c720_try_fmt(cap_dev, pix_mp);

	cap_dev->format.pix_mp = *pix_mp;
	cap_dev->format.info = mali_c720_get_v4l2_config_by_fourcc(pix_mp->pixelformat);
	info = cap_dev->format.info;

	memset(req, 0, sizeof(*req));
	req->type = MALI_C720_BUF_TYPE_OUT;
	req->sub_type = cap_dev->id;
	req->width = pix_mp->width;
	req->height = pix_mp->height;

	req->line_offset[0] = pix_mp->plane_fmt[0].bytesperline;
	req->plane_size[0] = pix_mp->plane_fmt[0].bytesperline * pix_mp->height;
	for (i = 1; i < info->comp_planes; i++) {
		req->line_offset[i] = pix_mp->plane_fmt[i].bytesperline;
		req->plane_size[i] = pix_mp->plane_fmt[i].sizeimage;
	}

	req->num_out_planes = info->comp_planes;
	req->buffer_cfg = info->buffer_cfg;
}

static int mali_c720_s_fmt_vid_cap_mplane(struct file *file, void *fh,
					  struct v4l2_format *f)
{
	struct mali_c720_cap_dev *cap_dev = video_drvdata(file);

	if (media_entity_is_streaming(&cap_dev->vdev.entity))
		return -EBUSY;

	if (vb2_is_busy(&cap_dev->queue))
		return -EBUSY;

	mali_c720_set_format(cap_dev, &f->fmt.pix_mp);
	return 0;
}

static int mali_c720_g_fmt_vid_cap_mplane(struct file *file, void *fh,
					  struct v4l2_format *f)
{
	struct mali_c720_cap_dev *cap_dev = video_drvdata(file);

	f->fmt.pix_mp = cap_dev->format.pix_mp;

	return 0;
}

static int mali_c720_enum_fmt_vid_cap_mplane(struct file *file, void *fh,
					     struct v4l2_fmtdesc *f)
{
	struct mali_c720_cap_dev *cap_dev = video_drvdata(file);
	const struct mali_c720_v4l2_format_info *info;
	struct mali_c720_v4l2_format_match match;

	match.dev_id = cap_dev->id;
	match.mbus_code = f->mbus_code;

	info = mali_c720_get_v4l2_config_by_matched_index(f->index, &match);
	if (info) {
		f->pixelformat = info->fourcc;
		return 0;
	}

	return -EINVAL;
}

static int mali_c720_enum_framesizes(struct file *file, void *fh,
				     struct v4l2_frmsizeenum *fsize)
{
	struct mali_c720_cap_dev *cap_dev = video_drvdata(file);
	const struct mali_c720_v4l2_format_info *info;

	if (fsize->index)
		return -EINVAL;

	info = mali_c720_get_v4l2_config_by_fourcc(fsize->pixel_format);
	if (!info || !mali_c720_format_supported_by_vdev(info, cap_dev->id))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = MALI_C720_MIN_OUTPUT_WIDTH;
	fsize->stepwise.max_width = MALI_C720_MAX_WIDTH;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.min_height = MALI_C720_MIN_OUTPUT_HEIGHT;
	fsize->stepwise.max_height = MALI_C720_MAX_HEIGHT;
	fsize->stepwise.step_height = 2;

	return 0;
}

static int mali_c720_querycap(struct file *file, void *fh,
			      struct v4l2_capability *cap)
{
	strscpy(cap->driver, MALI_C720_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ARM Mali-C720 ISP", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops mali_c720_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = mali_c720_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_try_fmt_vid_cap_mplane = mali_c720_try_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = mali_c720_s_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_cap_mplane = mali_c720_g_fmt_vid_cap_mplane,
	.vidioc_enum_fmt_vid_cap = mali_c720_enum_fmt_vid_cap_mplane,
	.vidioc_enum_framesizes = mali_c720_enum_framesizes,
	.vidioc_querycap = mali_c720_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int mali_c720_register_cap_dev(struct mali_c720 *mali_c720,
				      enum mali_c720_cap_devs cap_dev_id)
{
	struct mali_c720_cap_dev *cap_dev = &mali_c720->cap_devs[cap_dev_id];
	struct v4l2_pix_format_mplane pix_mp;
	struct video_device *vdev;
	struct vb2_queue *vb2q;
	int ret;

	vdev = &cap_dev->vdev;
	vb2q = &cap_dev->queue;

	cap_dev->id = cap_dev_id;
	cap_dev->mali_c720 = mali_c720;
	mutex_init(&cap_dev->lock);
	INIT_LIST_HEAD(&cap_dev->buffers.input);
	INIT_LIST_HEAD(&cap_dev->buffers.processing);
	spin_lock_init(&cap_dev->buffers.lock);

	memset(&pix_mp, 0, sizeof(pix_mp));

	pix_mp.width = MALI_C720_DEFAULT_WIDTH;
	pix_mp.height = MALI_C720_DEFAULT_HEIGHT;

	switch (cap_dev->id) {
	case MALI_C720_CAP_DEV_FR0_RGB:
		cap_dev->src_sd =
			&mali_c720->fr_pipe.mdev.sd;
		cap_dev->src_pad = MALI_C720_FR_PIPE_PAD_SOURCE_RGB;
		cap_dev->format.default_format = V4L2_PIX_FMT_RGB24;
		break;
	case MALI_C720_CAP_DEV_FR0_YUV:
		cap_dev->src_sd =
			&mali_c720->fr_pipe.mdev.sd;
		cap_dev->src_pad = MALI_C720_FR_PIPE_PAD_SOURCE_YUV;
		cap_dev->format.default_format = V4L2_PIX_FMT_NV12;
		break;
	default:
		dev_warn_once(mali_c720->dev,
			      "Invalid capture device id %u\n", cap_dev->id);
		ret = -EINVAL;
		goto err_destroy_mutex;
	}

	cap_dev->pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&cap_dev->vdev.entity, 1, &cap_dev->pad);
	if (ret)
		goto err_destroy_mutex;

	vb2q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
	vb2q->drv_priv = cap_dev;
	vb2q->mem_ops = &vb2_dma_contig_memops;
	vb2q->ops = &mali_c720_vb2_ops;
	vb2q->buf_struct_size = sizeof(struct mali_c720_cap_buffer);
	vb2q->min_queued_buffers = 0;
	vb2q->supports_requests = 1;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
				V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	vb2q->lock = &cap_dev->lock;
	vb2q->dev = mali_c720->dev;

	ret = vb2_queue_init(vb2q);
	if (ret) {
		dev_err(mali_c720->dev, "%s vb2 queue init failed\n",
			cap_dev->vdev.name);
		goto err_cleanup_media_entity;
	}

	strscpy(cap_dev->vdev.name, capture_device_names[cap_dev_id],
		sizeof(cap_dev->vdev.name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mali_c720_v4l2_fops;
	vdev->ioctl_ops = &mali_c720_v4l2_ioctl_ops;
	vdev->lock = &cap_dev->lock;
	vdev->v4l2_dev = &mali_c720->v4l2_dev;
	vdev->queue = &cap_dev->queue;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE |
				V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	vdev->entity.ops = &mali_c720_media_ops;
	video_set_drvdata(vdev, cap_dev);

	pix_mp.pixelformat = cap_dev->format.default_format;
	mali_c720_set_format(cap_dev, &pix_mp);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mali_c720->dev,
			"%s failed to register video device\n",
			cap_dev->vdev.name);
		goto err_release_vb2q;
	}

	return 0;

err_release_vb2q:
	vb2_queue_release(vb2q);
err_cleanup_media_entity:
	media_entity_cleanup(&cap_dev->vdev.entity);
err_destroy_mutex:
	mutex_destroy(&cap_dev->lock);
	cap_dev->mali_c720 = NULL;

	return ret;
}

int mali_c720_register_capture_devs(struct mali_c720 *mali_c720)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++) {
		ret = mali_c720_register_cap_dev(mali_c720, i);
		if (ret)
			return ret;
	}

	return 0;
}

static void mali_c720_unregister_cap_dev(struct mali_c720 *mali_c720,
					 enum mali_c720_cap_devs cap_dev_id)
{
	struct mali_c720_cap_dev *cap_dev = &mali_c720->cap_devs[cap_dev_id];

	if (!video_is_registered(&cap_dev->vdev))
		return;

	vb2_video_unregister_device(&cap_dev->vdev);
}

static void mali_c720_cleanup_cap_dev(struct mali_c720 *mali_c720,
				      enum mali_c720_cap_devs cap_dev_id)
{
	struct mali_c720_cap_dev *cap_dev = &mali_c720->cap_devs[cap_dev_id];

	if (!cap_dev->mali_c720)
		return;

	media_entity_cleanup(&cap_dev->vdev.entity);
	mutex_destroy(&cap_dev->lock);
	cap_dev->mali_c720 = NULL;
}

void mali_c720_unregister_capture_devs(struct mali_c720 *mali_c720)
{
	unsigned int i;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++)
		mali_c720_unregister_cap_dev(mali_c720, i);
}

void mali_c720_cleanup_capture_devs(struct mali_c720 *mali_c720)
{
	unsigned int i;

	for (i = 0; i < MALI_C720_MAX_NUM_CAP_DEVS; i++)
		mali_c720_cleanup_cap_dev(mali_c720, i);
}
