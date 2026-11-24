// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/dev_printk.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/media-bus-format.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-isp.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c720-common.h"

struct mali_c720_stats_discovery_info {
	u32 data_offset;
	u32 table_size;
	u32 nodes_offset;
};

static const struct mali_c720_stats_discovery_info
mali_c720_stats_discovery_info[MALI_C720_STATS_MAX] = {
	[MALI_C720_STATS_COLOR_HV] = {
		.data_offset = offsetof(struct mali_c720_stats_color, data),
		.table_size = offsetof(struct mali_c720_awb_statistics, nodes_used),
		.nodes_offset = offsetof(struct mali_c720_awb_statistics,
					 nodes_used.nodes_used_horiz),
	},
	[MALI_C720_STATS_HISTOGRAM_INP] = {
		.data_offset = offsetof(struct mali_c720_stats_histogram, data),
		.table_size = sizeof(struct mali_c720_ae_1024bin_hist),
	},
	[MALI_C720_STATS_HISTOGRAM_HV] = {
		.data_offset = offsetof(struct mali_c720_stats_histogram, data),
		.table_size = sizeof(struct mali_c720_ae_1024bin_hist),
	},
	[MALI_C720_STATS_HISTOGRAM_HV_IRIDIX] = {
		.data_offset = offsetof(struct mali_c720_stats_histogram, data),
		.table_size = sizeof(struct mali_c720_ae_1024bin_hist),
	},
};

static const struct mali_c720_discovery_target
mali_c720_stats_discovery_targets[MALI_C720_STATS_MAX] = {
	[MALI_C720_STATS_COLOR_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_COLOR_STATS, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_STATS_HISTOGRAM_INP] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_STATS_HISTOGRAM_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 1,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_STATS_HISTOGRAM_HV_IRIDIX] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 2,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
};

static const struct v4l2_isp_stats_block_type_info
mali_c720_stats_block_types_info[MALI_C720_STATS_MAX] = {
	[MALI_C720_STATS_COLOR_HV] = {
		.size = sizeof(struct mali_c720_stats_color),
	},
	[MALI_C720_STATS_HISTOGRAM_INP] = {
		.size = sizeof(struct mali_c720_stats_histogram),
	},
	[MALI_C720_STATS_HISTOGRAM_HV] = {
		.size = sizeof(struct mali_c720_stats_histogram),
	},
	[MALI_C720_STATS_HISTOGRAM_HV_IRIDIX] = {
		.size = sizeof(struct mali_c720_stats_histogram),
	},
	[MALI_C720_STATS_NOOP] = {
		.size = ALIGN(sizeof(struct v4l2_isp_buffer) +
			      sizeof(struct v4l2_isp_block_header), 16) -
			sizeof(struct v4l2_isp_buffer),
	},
};

static int
mali_c720_stats_fill_target(struct mali_c720 *mali_c720,
			    unsigned int block_type,
			    struct mali_c720_stats_target *target, u32 offset)
{
	const struct mali_c720_stats_discovery_info *info;
	const struct mali_c720_discovery_target *discovery;
	const struct mali_c720_stat_color_stats_regs *stat_color_stats =
		mali_c720->registers.modules->stat_color_stats;
	const struct mali_c720_reg_field *nodes_register = NULL;
	u32 module_base, table_base;
	u32 size_words;
	int ret;

	memset(target, 0, sizeof(*target));

	discovery = &mali_c720_stats_discovery_targets[block_type];

	if (!mali_c720_discovery_module_present(mali_c720, discovery))
		return -ENOENT;

	ret = mali_c720_discovery_get_module_table_base(mali_c720, discovery, 0,
							&table_base);
	if (ret)
		return ret;

	ret = mali_c720_discovery_get_module_base(mali_c720, discovery, &module_base);
	if (ret)
		return ret;

	info = &mali_c720_stats_discovery_info[block_type];

	offset += info->data_offset;
	size_words = info->table_size / sizeof(u32);
	/* Destination bits [3:2] are taken from the ISP address. */
	target->desc[0] = (struct mali_c720_cdma_sg_cfg) {
		.isp_address = table_base,
		.axi_address = offset >> 4,
		.length_low = (u16)(size_words & 0xfff),
		.length_high = (u8)((size_words >> 12) & 0xf),
		.alt_address = 1,
	};
	target->num_desc = 1;

	if (block_type == MALI_C720_STATS_COLOR_HV) {
		nodes_register = &stat_color_stats->awb_nodes_used_horiz;

		target->desc[1] = (struct mali_c720_cdma_sg_cfg) {
			.isp_address = mali_c720_reg_addr(module_base, nodes_register),
			.axi_address = (offset + info->nodes_offset) >> 4,
			.length_low = 1,
			.alt_address = 1,
		};
		target->num_desc++;
	}

	target->present = true;

	return 0;
}

static int mali_c720_stats_build_target_cache(struct mali_c720 *mali_c720)
{
	struct mali_c720_stats *stats = &mali_c720->stats;
	unsigned int block_type;
	int ret;

	/* The NOOP aligns the first statistics payload for the CDMA engine. */
	stats->buffer_size =
		v4l2_isp_buffer_size(mali_c720_stats_block_types_info[MALI_C720_STATS_NOOP].size);
	stats->num_targets = 0;

	for (block_type = 0; block_type < MALI_C720_STATS_MAX; block_type++) {
		if (block_type == MALI_C720_STATS_NOOP)
			continue;

		ret = mali_c720_stats_fill_target(mali_c720, block_type,
						  &stats->targets[block_type], stats->buffer_size);
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;

		stats->buffer_size += (u32)mali_c720_stats_block_types_info[block_type].size;
		stats->num_targets++;
	}

	if (stats->buffer_size > v4l2_isp_buffer_size(MALI_C720_STATS_MAX_SIZE)) {
		dev_err(mali_c720->dev, "statistics buffer exceeds maximum size\n");
		return -EINVAL;
	}

	if (!stats->num_targets) {
		dev_err(mali_c720->dev, "no supported statistics blocks found\n");
		return -ENODEV;
	}

	return 0;
}

static int mali_c720_stats_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_c720_stats *stats =
		container_of(ctrl->handler, struct mali_c720_stats, ctrls.handler);
	u16 *ids = ctrl->p_new.p_u16;
	unsigned int block_type;

	if (ctrl->id != V4L2_CID_MALI_C720_STATS_CAPABILITIES)
		return -EINVAL;

	for (block_type = 0; block_type < MALI_C720_STATS_MAX; block_type++) {
		if (stats->targets[block_type].present)
			*ids++ = (u16)block_type;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mali_c720_stats_ctrl_ops = {
	.g_volatile_ctrl = mali_c720_stats_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config mali_c720_stats_caps_ctrl = {
	.ops = &mali_c720_stats_ctrl_ops,
	.id = V4L2_CID_MALI_C720_STATS_CAPABILITIES,
	.name = "Mali-C720 Stats Capabilities",
	.type = V4L2_CTRL_TYPE_U16,
	.dims = { MALI_C720_STATS_MAX },
	.min = 0,
	.max = U16_MAX,
	.step = 1,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

static int mali_c720_stats_init_buffer(struct mali_c720_stats *stats, void *vaddr)
{
	struct mali_c720 *mali_c720 = stats->mali_c720;
	const unsigned int num_types = ARRAY_SIZE(mali_c720_stats_block_types_info);
	struct v4l2_isp_buffer *buffer = vaddr;
	struct v4l2_isp_block_header *header;
	size_t max_size = stats->buffer_size - sizeof(*buffer);
	unsigned int block_type;

	memset(buffer, 0, stats->buffer_size);
	v4l2_isp_stats_init_buffer(buffer, V4L2_ISP_VERSION_V1);

	header = v4l2_isp_stats_init_block(mali_c720->dev, buffer,
					   mali_c720_stats_block_types_info,
					   num_types, MALI_C720_STATS_NOOP, max_size);
	if (IS_ERR(header))
		return (int)PTR_ERR(header);

	for (block_type = 0; block_type < MALI_C720_STATS_MAX; block_type++) {
		if (!stats->targets[block_type].present)
			continue;

		header = v4l2_isp_stats_init_block(mali_c720->dev, buffer,
						   mali_c720_stats_block_types_info,
						   num_types, block_type, max_size);
		if (IS_ERR(header))
			return (int)PTR_ERR(header);
	}

	return 0;
}

static int mali_c720_stats_enum_fmt_meta_cap(struct file *file, void *fh,
					     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	if (f->mbus_code && f->mbus_code != MEDIA_BUS_FMT_METADATA_FIXED)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_MALI_C720_STATS;

	return 0;
}

static int mali_c720_stats_g_fmt_meta_cap(struct file *file, void *fh,
					  struct v4l2_format *f)
{
	struct mali_c720_stats *stats = video_drvdata(file);

	const struct v4l2_meta_format mfmt = {
		.dataformat = V4L2_META_FMT_MALI_C720_STATS,
		.buffersize = stats->buffer_size
	};

	f->fmt.meta = mfmt;

	return 0;
}

static int mali_c720_stats_querycap(struct file *file,
				    void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, MALI_C720_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ARM Mali-C720 ISP", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops mali_c720_stats_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = mali_c720_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_cap = mali_c720_stats_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = mali_c720_stats_g_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = mali_c720_stats_g_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = mali_c720_stats_g_fmt_meta_cap,
	.vidioc_querycap = mali_c720_stats_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mali_c720_stats_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int
mali_c720_stats_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			    unsigned int *num_planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct mali_c720_stats *stats = vb2_get_drv_priv(q);

	if (*num_planes && *num_planes > 1)
		return -EINVAL;

	if (sizes[0] && sizes[0] < stats->buffer_size)
		return -EINVAL;

	*num_planes = 1;

	if (!sizes[0])
		sizes[0] = stats->buffer_size;

	return 0;
}

static void mali_c720_stats_buf_queue(struct vb2_buffer *vb)
{
	struct mali_c720_stats *stats = vb2_get_drv_priv(vb->vb2_queue);
	struct mali_c720 *mali_c720 = stats->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_stats_buf *buf = container_of(vbuf, struct mali_c720_stats_buf, vb);

	vb2_set_plane_payload(vb, 0, stats->buffer_size);

	scoped_guard(spinlock, &stats->buffers.lock)
		list_add_tail(&buf->queue, &stats->buffers.input);

	if (!mali_c720_stats_uses_requests(stats))
		mali_c720_try_schedule_frame(ctx);
}

static int mali_c720_stats_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_stats_buf *buf = container_of(vbuf, struct mali_c720_stats_buf, vb);

	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	if (!IS_ALIGNED(buf->addr, 16)) {
		dev_dbg(vb->vb2_queue->dev,
			"Statistics buffer DMA address is not 16-byte aligned\n");
		return -EINVAL;
	}

	return 0;
}

static int mali_c720_stats_buf_prepare(struct vb2_buffer *vb)
{
	struct mali_c720_stats *stats = vb2_get_drv_priv(vb->vb2_queue);
	struct dma_buf *dbuf = vb->planes[0].dbuf;
	void *vaddr;
	int ret;

	vaddr = vb2_plane_vaddr(vb, 0);
	if (!vaddr)
		return -EFAULT;

	if (dbuf) {
		ret = dma_buf_begin_cpu_access(dbuf, DMA_BIDIRECTIONAL);
		if (ret)
			return ret;
	}

	ret = mali_c720_stats_init_buffer(stats, vaddr);

	if (dbuf) {
		int err = dma_buf_end_cpu_access(dbuf, DMA_BIDIRECTIONAL);

		if (!ret)
			ret = err;
	}

	return ret;
}

static void mali_c720_buf_request_complete(struct vb2_buffer *vb)
{
}

bool mali_c720_stats_uses_requests(struct mali_c720_stats *stats)
{
	return video_is_registered(&stats->vdev) &&
	       vb2_is_streaming(&stats->queue) &&
	       stats->queue.uses_requests;
}

struct media_request *mali_c720_stats_get_request(struct mali_c720_stats *stats)
{
	struct mali_c720_stats_buf *buf;

	if (!mali_c720_stats_uses_requests(stats))
		return NULL;

	guard(spinlock)(&stats->buffers.lock);

	buf = list_first_entry_or_null(&stats->buffers.input,
				       struct mali_c720_stats_buf, queue);

	return buf ? buf->vb.vb2_buf.request : NULL;
}

int mali_c720_stats_is_ready(struct mali_c720_stats *stats)
{
	struct mali_c720_stats_buf *buf;

	if (!video_is_registered(&stats->vdev))
		return -ENODEV;

	if (!vb2_is_streaming(&stats->queue))
		return -ENODEV;

	scoped_guard(spinlock, &stats->buffers.lock) {
		buf = list_first_entry_or_null(&stats->buffers.input,
					       struct mali_c720_stats_buf, queue);
		if (!buf)
			return -ENOENT;
	}

	return 0;
}

void mali_c720_stats_set_next_buffer(struct mali_c720_stats *stats,
				     struct media_request *req)
{
	struct mali_c720 *mali_c720 = stats->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_stats_buf *buf;

	if (!video_is_registered(&stats->vdev))
		return;

	if (!vb2_is_streaming(&stats->queue))
		return;

	scoped_guard(spinlock, &stats->buffers.lock) {
		buf = list_first_entry_or_null(&stats->buffers.input,
					       struct mali_c720_stats_buf, queue);
		if (buf && (!buf->vb.vb2_buf.request ||
			    buf->vb.vb2_buf.request == req)) {
			mali_c720_ctx_mgr_set_stats_buffer_address(ctx, &buf->addr);
			list_move_tail(&buf->queue, &stats->buffers.processing);
		} else {
			mali_c720_ctx_mgr_set_stats_buffer_address(ctx, NULL);
		}
	}
}

void mali_c720_stats_buffer_done(struct mali_c720_stats *stats, bool success)
{
	struct mali_c720 *mali_c720 = stats->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_stats_buf *buf;

	if (!video_is_registered(&stats->vdev))
		return;

	if (!vb2_is_streaming(&stats->queue))
		return;

	guard(spinlock)(&stats->buffers.lock);

	buf = list_first_entry_or_null(&stats->buffers.processing,
				       struct mali_c720_stats_buf, queue);
	if (!buf)
		return;

	if (success) {
		buf->vb.vb2_buf.timestamp = ctx->timestamp;
		buf->vb.sequence = ctx->frame_sequence;
	}

	vb2_buffer_done(&buf->vb.vb2_buf, success ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	list_del(&buf->queue);
}

static void mali_c720_stats_return_buffers(struct mali_c720_stats *stats,
					   enum vb2_buffer_state state)
{
	struct mali_c720_stats_buf *buf, *tmp;

	guard(spinlock) (&stats->buffers.lock);

	list_for_each_entry_safe(buf, tmp, &stats->buffers.input, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	list_for_each_entry_safe(buf, tmp, &stats->buffers.processing, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static int mali_c720_stats_start_streaming(struct vb2_queue *q,
					   unsigned int count)
{
	struct mali_c720_stats *stats = vb2_get_drv_priv(q);
	struct mali_c720 *mali_c720 = stats->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct v4l2_subdev *isp_sd = &mali_c720->isp.mdev.sd;
	int ret;

	guard(mutex) (&mali_c720->device_lock);

	if (!media_devnode_is_registered(mali_c720->media_dev.devnode)) {
		ret = -ENODEV;
		goto err_return_buffers;
	}

	ret = pm_runtime_resume_and_get(mali_c720->dev);
	if (ret)
		goto err_return_buffers;

	ret = video_device_pipeline_start(&stats->vdev, &mali_c720->pipe);
	if (ret)
		goto err_pm_put;

	ret = mali_c720_alloc_context(ctx);
	if (ret)
		goto err_stop_pipeline;

	ret = mali_c720_pipeline_ready(mali_c720);
	if (ret && ret != -EAGAIN)
		goto err_free_context;

	if (!ret) {
		ret = v4l2_subdev_enable_streams(isp_sd,
						 MALI_C720_ISP_PAD_SOURCE_HV,
						 BIT(0));
		if (ret)
			goto err_free_context;
	}

	return 0;

err_free_context:
	mali_c720_free_context(ctx);
err_stop_pipeline:
	video_device_pipeline_stop(&stats->vdev);
err_pm_put:
	pm_runtime_put(mali_c720->dev);
err_return_buffers:
	mali_c720_stats_return_buffers(stats, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void mali_c720_stats_stop_streaming(struct vb2_queue *q)
{
	struct mali_c720_stats *stats = vb2_get_drv_priv(q);
	struct mali_c720 *mali_c720 = stats->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct v4l2_subdev *isp_sd = &isp->mdev.sd;

	guard(mutex) (&mali_c720->device_lock);

	v4l2_subdev_disable_streams(isp_sd, MALI_C720_ISP_PAD_SOURCE_HV,
				    BIT(0));
	mali_c720_stats_return_buffers(stats, VB2_BUF_STATE_ERROR);

	mali_c720_free_context(ctx);

	video_device_pipeline_stop(&stats->vdev);

	pm_runtime_put_autosuspend(mali_c720->dev);
}

static const struct vb2_ops mali_c720_stats_vb2_ops = {
	.queue_setup = mali_c720_stats_queue_setup,
	.buf_queue = mali_c720_stats_buf_queue,
	.buf_init = mali_c720_stats_buf_init,
	.buf_prepare = mali_c720_stats_buf_prepare,
	.buf_request_complete = mali_c720_buf_request_complete,
	.start_streaming = mali_c720_stats_start_streaming,
	.stop_streaming = mali_c720_stats_stop_streaming,
};

void mali_c720_unregister_stats(struct mali_c720 *mali_c720)
{
	struct mali_c720_stats *stats = &mali_c720->stats;

	if (!video_is_registered(&stats->vdev))
		return;

	vb2_video_unregister_device(&stats->vdev);
}

void mali_c720_cleanup_stats(struct mali_c720 *mali_c720)
{
	struct mali_c720_stats *stats = &mali_c720->stats;

	if (!stats->mali_c720)
		return;

	v4l2_ctrl_handler_free(&stats->ctrls.handler);
	media_entity_cleanup(&stats->vdev.entity);

	mutex_destroy(&stats->lock);
	stats->mali_c720 = NULL;
}

int mali_c720_register_stats(struct mali_c720 *mali_c720)
{
	struct mali_c720_stats *stats = &mali_c720->stats;
	struct video_device *vdev = &stats->vdev;
	struct vb2_queue *vb2q = &stats->queue;
	u32 dims[V4L2_CTRL_MAX_DIMS] = { 0 };
	int ret;

	ret = mali_c720_stats_build_target_cache(mali_c720);
	if (ret)
		return ret;

	ret = mali_c720_ctx_mgr_prepare_cdma_sg_descriptors(mali_c720);
	if (ret)
		return ret;

	stats->mali_c720 = mali_c720;
	mutex_init(&stats->lock);
	INIT_LIST_HEAD(&stats->buffers.input);
	INIT_LIST_HEAD(&stats->buffers.processing);
	spin_lock_init(&stats->buffers.lock);

	ret = v4l2_ctrl_handler_init(&stats->ctrls.handler, 1);
	if (ret)
		goto err_destroy_mutex;

	stats->ctrls.stats_capabilities =
		v4l2_ctrl_new_custom(&stats->ctrls.handler,
				     &mali_c720_stats_caps_ctrl, NULL);
	if (stats->ctrls.stats_capabilities) {
		dims[0] = stats->num_targets;
		ret = v4l2_ctrl_modify_dimensions(stats->ctrls.stats_capabilities, dims);
		if (ret)
			goto err_free_ctrl_handler;
	}

	if (stats->ctrls.handler.error) {
		ret = stats->ctrls.handler.error;
		goto err_free_ctrl_handler;
	}

	stats->pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&stats->vdev.entity, 1, &stats->pad);
	if (ret)
		goto err_free_ctrl_handler;

	vb2q->type = V4L2_BUF_TYPE_META_CAPTURE;
	vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
	vb2q->drv_priv = stats;
	vb2q->mem_ops = &vb2_dma_contig_memops;
	vb2q->bidirectional = 1;
	vb2q->ops = &mali_c720_stats_vb2_ops;
	vb2q->buf_struct_size = sizeof(struct mali_c720_stats_buf);
	vb2q->min_queued_buffers = 0;
	vb2q->supports_requests = 1;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC |
				V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	vb2q->lock = &stats->lock;
	vb2q->dev = mali_c720->dev;

	ret = vb2_queue_init(vb2q);
	if (ret) {
		dev_err(mali_c720->dev, "stats vb2 queue init failed\n");
		goto err_cleanup_entity;
	}

	strscpy(stats->vdev.name, "mali-c720 3a stats", sizeof(stats->vdev.name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mali_c720_stats_v4l2_fops;
	vdev->ioctl_ops = &mali_c720_stats_v4l2_ioctl_ops;
	vdev->lock = &stats->lock;
	vdev->v4l2_dev = &mali_c720->v4l2_dev;
	vdev->queue = &stats->queue;
	vdev->ctrl_handler = &stats->ctrls.handler;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	vdev->vfl_dir = VFL_DIR_RX;
	video_set_drvdata(vdev, stats);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mali_c720->dev,
			"failed to register stats video device\n");
		goto err_release_vb2q;
	}

	return 0;

err_release_vb2q:
	vb2_queue_release(vb2q);
err_cleanup_entity:
	media_entity_cleanup(&stats->vdev.entity);
err_free_ctrl_handler:
	v4l2_ctrl_handler_free(&stats->ctrls.handler);
err_destroy_mutex:
	mutex_destroy(&stats->lock);
	stats->mali_c720 = NULL;

	return ret;
}
