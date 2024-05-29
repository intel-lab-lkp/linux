// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Configuration parameters output device
 *
 * Copyright (C) 2024 Ideas on Board Oy
 */
#include <linux/media/arm/mali-c55-config.h>

#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"

typedef void (*mali_c55_block_handler)(struct mali_c55 *mali_c55,
				       struct mali_c55_params_block_header *block);

struct mali_c55_block_handler {
	size_t size;
	mali_c55_block_handler handler;
};

static void mali_c55_params_sensor_offs(struct mali_c55 *mali_c55,
					struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_sensor_off_preshading *p =
		(struct mali_c55_params_sensor_off_preshading *)block;
	__u32 global_offset;

	if (!block->enabled)
		return;

	if (!(p->chan00 || p->chan01 || p->chan10 || p->chan11))
		return;

	mali_c55_write(mali_c55, MALI_C55_REG_SENSOR_OFF_PRE_SHA_00,
		       p->chan00 & MALI_C55_SENSOR_OFF_PRE_SHA_MASK);
	mali_c55_write(mali_c55, MALI_C55_REG_SENSOR_OFF_PRE_SHA_01,
		       p->chan01 & MALI_C55_SENSOR_OFF_PRE_SHA_MASK);
	mali_c55_write(mali_c55, MALI_C55_REG_SENSOR_OFF_PRE_SHA_10,
		       p->chan10 & MALI_C55_SENSOR_OFF_PRE_SHA_MASK);
	mali_c55_write(mali_c55, MALI_C55_REG_SENSOR_OFF_PRE_SHA_11,
		       p->chan11 & MALI_C55_SENSOR_OFF_PRE_SHA_MASK);

	/*
	 * The average offset is applied as a global offset for the digital
	 * gain block
	 */
	global_offset = (p->chan00 + p->chan01 + p->chan10 + p->chan11) >> 2;
	mali_c55_update_bits(mali_c55, MALI_C55_REG_DIGITAL_GAIN_OFFSET,
			     MALI_C55_DIGITAL_GAIN_OFFSET_MASK, global_offset);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_BYPASS_3,
			     MALI_C55_REG_BYPASS_3_SENSOR_OFFSET_PRE_SH, 0x00);
}

static void mali_c55_params_aexp_hist(struct mali_c55 *mali_c55,
				struct mali_c55_params_block_header *block)
{
	u32 disable_mask = block->type == MALI_C55_PARAM_BLOCK_AEXP_HIST ?
					  MALI_C55_AEXP_HIST_DISABLE_MASK :
					  MALI_C55_AEXP_IHIST_DISABLE_MASK;
	u32 base = block->type == MALI_C55_PARAM_BLOCK_AEXP_HIST ?
				  MALI_C55_REG_AEXP_HIST_BASE :
				  MALI_C55_REG_AEXP_IHIST_BASE;
	struct mali_c55_params_aexp_hist *params =
		(struct mali_c55_params_aexp_hist *)block;

	if (!block->enabled) {
		mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
				     disable_mask, true);
		return;
	}

	mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
			     disable_mask, false);

	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SKIP_OFFSET,
			     MALI_C55_AEXP_HIST_SKIP_X_MASK, params->skip_x);
	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SKIP_OFFSET,
			     MALI_C55_AEXP_HIST_OFFSET_X_MASK, params->offset_x);
	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SKIP_OFFSET,
			     MALI_C55_AEXP_HIST_SKIP_Y_MASK, params->skip_y);
	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SKIP_OFFSET,
			     MALI_C55_AEXP_HIST_OFFSET_Y_MASK, params->offset_y);

	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SCALE_OFFSET,
			     MALI_C55_AEXP_HIST_SCALE_BOTTOM_MASK, params->scale_bottom);
	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_SCALE_OFFSET,
			     MALI_C55_AEXP_HIST_SCALE_TOP_MASK, params->scale_top);

	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_PLANE_MODE_OFFSET,
			     MALI_C55_AEXP_HIST_PLANE_MODE_MASK, params->plane_mode);

	if (block->type == MALI_C55_PARAM_BLOCK_AEXP_HIST)
		mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
				     MALI_C55_AEXP_HIST_SWITCH_MASK,
				     params->tap_point);
}

static void
mali_c55_params_aexp_hist_weights(struct mali_c55 *mali_c55,
				  struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_aexp_weights *params =
		(struct mali_c55_params_aexp_weights *)block;
	u32 base;

	if (!block->enabled)
		return;

	base = block->type == MALI_C55_PARAM_BLOCK_AEXP_HIST_WEIGHTS ?
			      MALI_C55_REG_AEXP_HIST_BASE :
			      MALI_C55_REG_AEXP_IHIST_BASE;

	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_NODES_USED_OFFSET,
			     MALI_C55_AEXP_HIST_NODES_USED_HORIZ_MASK, params->nodes_used_horiz);
	mali_c55_update_bits(mali_c55, base + MALI_C55_AEXP_HIST_NODES_USED_OFFSET,
			     MALI_C55_AEXP_HIST_NODES_USED_VERT_MASK, params->nodes_used_vert);

	/*
	 * The zone weights array is a 225-element array of u8 values, but that
	 * is a bit annoying to handle given the ISP expects 32-bit writes. We
	 * just reinterpret it as a 57-element array of 32-bit values for the
	 * purposes of this transaction (the 3 bytes of additional space at the
	 * end of the write is just padding for the array of weights in the ISP
	 * memory space anyway, so there's no risk of overwriting other
	 * registers).
	 */
	for (unsigned int i = 0; i < 57; i++) {
		u32 val = ((u32 *)params->zone_weights)[i]
			    & MALI_C55_AEXP_HIST_ZONE_WEIGHT_MASK;
		u32 addr = base + MALI_C55_AEXP_HIST_ZONE_WEIGHTS_OFFSET + (4 * i);

		mali_c55_write(mali_c55, addr, val);
	}
}

static void mali_c55_params_digital_gain(struct mali_c55 *mali_c55,
					 struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_digital_gain *dgain =
		(struct mali_c55_params_digital_gain *)block;

	/*
	 * If the block is flagged as disabled we write a gain of 1.0, which in
	 * Q5.8 format is 256.
	 */
	mali_c55_update_bits(mali_c55, MALI_C55_REG_DIGITAL_GAIN,
			     MALI_C55_DIGITAL_GAIN_MASK,
			     block->enabled ? dgain->gain : 256);
}

static void mali_c55_params_awb_gains(struct mali_c55 *mali_c55,
				      struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_awb_gains *gains =
		(struct mali_c55_params_awb_gains *)block;

	/*
	 * There are two places AWB gains can be set in the ISP; one affects the
	 * image output data and the other affects the statistics for the
	 * AEXP-0 tap point.
	 */
	u32 addr1 = block->type = MALI_C55_PARAM_BLOCK_AWB_GAINS ?
				  MALI_C55_REG_AWB_GAINS1 :
				  MALI_C55_REG_AWB_GAINS1_AEXP;
	u32 addr2 = block->type = MALI_C55_PARAM_BLOCK_AWB_GAINS ?
				  MALI_C55_REG_AWB_GAINS2 :
				  MALI_C55_REG_AWB_GAINS2_AEXP;

	mali_c55_update_bits(mali_c55, addr1, MALI_C55_AWB_GAIN00_MASK,
			     gains->gain00);
	mali_c55_update_bits(mali_c55, addr1, MALI_C55_AWB_GAIN01_MASK,
			     gains->gain01);
	mali_c55_update_bits(mali_c55, addr2, MALI_C55_AWB_GAIN10_MASK,
			     gains->gain10);
	mali_c55_update_bits(mali_c55, addr2, MALI_C55_AWB_GAIN11_MASK,
			     gains->gain11);
}

static void mali_c55_params_awb_config(struct mali_c55 *mali_c55,
				      struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_awb_config *params =
		(struct mali_c55_params_awb_config *)block;

	if (!block->enabled) {
		mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
				     MALI_C55_AWB_DISABLE_MASK, true);
		return;
	}

	mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
			     MALI_C55_AWB_DISABLE_MASK, false);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_STATS_MODE,
			     MALI_C55_AWB_STATS_MODE_MASK, params->stats_mode);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_WHITE_LEVEL,
			     MALI_C55_AWB_WHITE_LEVEL_MASK, params->white_level);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_BLACK_LEVEL,
			     MALI_C55_AWB_BLACK_LEVEL_MASK, params->black_level);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CR_MAX,
			     MALI_C55_AWB_CR_MAX_MASK, params->cr_max);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CR_MIN,
			     MALI_C55_AWB_CR_MIN_MASK, params->cr_min);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CB_MAX,
			     MALI_C55_AWB_CB_MAX_MASK, params->cb_max);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CB_MIN,
			     MALI_C55_AWB_CB_MIN_MASK, params->cb_min);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_NODES_USED,
			     MALI_C55_AWB_NODES_USED_HORIZ_MASK,
			     params->nodes_used_horiz);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_NODES_USED,
			     MALI_C55_AWB_NODES_USED_VERT_MASK,
			     params->nodes_used_vert);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CR_HIGH,
			     MALI_C55_AWB_CR_HIGH_MASK, params->cr_high);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CR_LOW,
			     MALI_C55_AWB_CR_LOW_MASK, params->cr_low);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CB_HIGH,
			     MALI_C55_AWB_CB_HIGH_MASK, params->cb_high);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_AWB_CB_LOW,
			     MALI_C55_AWB_CB_LOW_MASK, params->cb_low);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_METERING_CONFIG,
			     MALI_C55_AWB_SWITCH_MASK, params->tap_point);
}

static void mali_c55_params_lsc_config(struct mali_c55 *mali_c55,
				       struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_mesh_shading_config *params =
		(struct mali_c55_params_mesh_shading_config *)block;
	unsigned int i;
	u32 addr;

	if (!block->enabled) {
		mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
				     MALI_C55_MESH_SHADING_ENABLE_MASK, false);
		return;
	}

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_ENABLE_MASK, true);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_MESH_SHOW, params->mesh_show);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_SCALE_MASK,
			     params->mesh_scale);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_PAGE_R_MASK,
			     params->mesh_page_r);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_PAGE_G_MASK,
			     params->mesh_page_g);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_PAGE_B_MASK,
			     params->mesh_page_b);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_MESH_WIDTH_MASK,
			     params->mesh_width);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_CONFIG,
			     MALI_C55_MESH_SHADING_MESH_HEIGHT_MASK,
			     params->mesh_height);

	for (i = 0; i < MALI_C55_NUM_MESH_SHADING_ELEMENTS; i++) {
		addr = MALI_C55_REG_MESH_SHADING_TABLES + (i * 4);
		mali_c55_write(mali_c55, addr, params->mesh[i]);
	}
}

static void mali_c55_params_lsc_selection(struct mali_c55 *mali_c55,
					  struct mali_c55_params_block_header *block)
{
	struct mali_c55_params_mesh_shading_selection *params =
		(struct mali_c55_params_mesh_shading_selection *)block;

	if (!block->enabled)
		return;

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA_BANK,
			     MALI_C55_MESH_SHADING_ALPHA_BANK_R_MASK,
			     params->mesh_alpha_bank_r);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA_BANK,
			     MALI_C55_MESH_SHADING_ALPHA_BANK_G_MASK,
			     params->mesh_alpha_bank_g);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA_BANK,
			     MALI_C55_MESH_SHADING_ALPHA_BANK_B_MASK,
			     params->mesh_alpha_bank_b);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA,
			     MALI_C55_MESH_SHADING_ALPHA_R_MASK,
			     params->mesh_alpha_r);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA,
			     MALI_C55_MESH_SHADING_ALPHA_G_MASK,
			     params->mesh_alpha_g);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_ALPHA,
			     MALI_C55_MESH_SHADING_ALPHA_B_MASK,
			     params->mesh_alpha_b);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MESH_SHADING_MESH_STRENGTH,
			     MALI_c55_MESH_STRENGTH_MASK,
			     params->mesh_strength);
}

static const struct mali_c55_block_handler mali_c55_block_handlers[] = {
	[MALI_C55_PARAM_BLOCK_SENSOR_OFFS] = {
		.size = sizeof(struct mali_c55_params_sensor_off_preshading),
		.handler = &mali_c55_params_sensor_offs,
	},
	[MALI_C55_PARAM_BLOCK_AEXP_HIST] = {
		.size = sizeof(struct mali_c55_params_aexp_hist),
		.handler = &mali_c55_params_aexp_hist,
	},
	[MALI_C55_PARAM_BLOCK_AEXP_IHIST] = {
		.size = sizeof(struct mali_c55_params_aexp_hist),
		.handler = &mali_c55_params_aexp_hist,
	},
	[MALI_C55_PARAM_BLOCK_AEXP_HIST_WEIGHTS] = {
		.size = sizeof(struct mali_c55_params_aexp_weights),
		.handler = &mali_c55_params_aexp_hist_weights,
	},
	[MALI_C55_PARAM_BLOCK_AEXP_IHIST_WEIGHTS] = {
		.size = sizeof(struct mali_c55_params_aexp_weights),
		.handler = &mali_c55_params_aexp_hist_weights,
	},
	[MALI_C55_PARAM_BLOCK_DIGITAL_GAIN] = {
		.size = sizeof(struct mali_c55_params_digital_gain),
		.handler = &mali_c55_params_digital_gain,
	},
	[MALI_C55_PARAM_BLOCK_AWB_GAINS] = {
		.size = sizeof(struct mali_c55_params_awb_gains),
		.handler = &mali_c55_params_awb_gains,
	},
	[MALI_C55_PARAM_BLOCK_AWB_CONFIG] = {
		.size = sizeof(struct mali_c55_params_awb_config),
		.handler = &mali_c55_params_awb_config,
	},
	[MALI_C55_PARAM_BLOCK_AWB_GAINS_AEXP] = {
		.size = sizeof(struct mali_c55_params_awb_gains),
		.handler = &mali_c55_params_awb_gains,
	},
	[MALI_C55_PARAM_MESH_SHADING_CONFIG] = {
		.size = sizeof(struct mali_c55_params_mesh_shading_config),
		.handler = &mali_c55_params_lsc_config,
	},
	[MALI_C55_PARAM_MESH_SHADING_SELECTION] = {
		.size = sizeof(struct mali_c55_params_mesh_shading_selection),
		.handler = &mali_c55_params_lsc_selection,
	},
};

static int mali_c55_params_enum_fmt_meta_out(struct file *file, void *fh,
					    struct v4l2_fmtdesc *f)
{
	if (f->index || f->type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_MALI_C55_PARAMS;

	return 0;
}

static int mali_c55_params_g_fmt_meta_out(struct file *file, void *fh,
					 struct v4l2_format *f)
{
	static const struct v4l2_meta_format mfmt = {
		.dataformat = V4L2_META_FMT_MALI_C55_PARAMS,
		.buffersize = sizeof(struct mali_c55_params_buffer),
	};

	if (f->type != V4L2_BUF_TYPE_META_OUTPUT)
		return -EINVAL;

	f->fmt.meta = mfmt;

	return 0;
}

static int mali_c55_params_querycap(struct file *file,
				   void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, MALI_C55_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ARM Mali-C55 ISP", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops mali_c55_params_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_out = mali_c55_params_enum_fmt_meta_out,
	.vidioc_g_fmt_meta_out = mali_c55_params_g_fmt_meta_out,
	.vidioc_s_fmt_meta_out = mali_c55_params_g_fmt_meta_out,
	.vidioc_try_fmt_meta_out = mali_c55_params_g_fmt_meta_out,
	.vidioc_querycap = mali_c55_params_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mali_c55_params_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int
mali_c55_params_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			   unsigned int *num_planes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	if (*num_planes && *num_planes > 1)
		return -EINVAL;

	if (sizes[0] && sizes[0] != sizeof(struct mali_c55_params_buffer))
		return -EINVAL;

	*num_planes = 1;
	sizes[0] = sizeof(struct mali_c55_params_buffer);

	return 0;
}

static void mali_c55_params_buf_queue(struct vb2_buffer *vb)
{
	struct mali_c55_params *params = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c55_buffer *buf = container_of(vbuf,
						   struct mali_c55_buffer, vb);

	vb2_set_plane_payload(vb, 0, sizeof(struct mali_c55_params_buffer));

	spin_lock(&params->buffers.lock);
	list_add_tail(&buf->queue, &params->buffers.queue);
	spin_unlock(&params->buffers.lock);
}

static void mali_c55_params_stop_streaming(struct vb2_queue *q)
{
	struct mali_c55_params *params = vb2_get_drv_priv(q);
	struct mali_c55_buffer *buf, *tmp;

	spin_lock(&params->buffers.lock);

	list_for_each_entry_safe(buf, tmp, &params->buffers.queue, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	spin_unlock(&params->buffers.lock);
}

static const struct vb2_ops mali_c55_params_vb2_ops = {
	.queue_setup = mali_c55_params_queue_setup,
	.buf_queue = mali_c55_params_buf_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.stop_streaming = mali_c55_params_stop_streaming,
};

void mali_c55_params_write_config(struct mali_c55 *mali_c55)
{
	struct mali_c55_params *params = &mali_c55->params;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	struct mali_c55_params_buffer *config;
	struct mali_c55_buffer *buf;
	size_t block_offset = 0;

	spin_lock(&params->buffers.lock);

	buf = list_first_entry_or_null(&params->buffers.queue,
				       struct mali_c55_buffer, queue);
	if (buf)
		list_del(&buf->queue);
	spin_unlock(&params->buffers.lock);

	if (!buf)
		return;

	buf->vb.sequence = mali_c55->isp.frame_sequence;
	config = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

	if (config->total_size > MALI_C55_PARAMS_MAX_SIZE) {
		dev_dbg(mali_c55->dev, "Invalid parameters buffer size %lu\n",
			config->total_size);
		state = VB2_BUF_STATE_ERROR;
		goto err_buffer_done;
	}

	/* Walk the list of parameter blocks and process them. */
	while (block_offset < config->total_size) {
		const struct mali_c55_block_handler *block_handler;
		struct mali_c55_params_block_header *block;

		block = (struct mali_c55_params_block_header *)
			 &config->data[block_offset];

		if (block->type >= MALI_C55_PARAM_BLOCK_SENTINEL) {
			dev_dbg(mali_c55->dev, "Invalid parameters block type\n");
			state = VB2_BUF_STATE_ERROR;
			goto err_buffer_done;
		}

		block_handler = &mali_c55_block_handlers[block->type];
		if (block->size != block_handler->size) {
			dev_dbg(mali_c55->dev, "Invalid parameters block size\n");
			state = VB2_BUF_STATE_ERROR;
			goto err_buffer_done;
		}

		block_handler->handler(mali_c55, block);

		block_offset += block->size;
	}

err_buffer_done:
	vb2_buffer_done(&buf->vb.vb2_buf, state);
}

void mali_c55_unregister_params(struct mali_c55 *mali_c55)
{
	struct mali_c55_params *params = &mali_c55->params;

	if (!video_is_registered(&params->vdev))
		return;

	vb2_video_unregister_device(&params->vdev);
	media_entity_cleanup(&params->vdev.entity);
	mutex_destroy(&params->lock);
}

int mali_c55_register_params(struct mali_c55 *mali_c55)
{
	struct mali_c55_params *params = &mali_c55->params;
	struct video_device *vdev = &params->vdev;
	struct vb2_queue *vb2q = &params->queue;
	int ret;

	mutex_init(&params->lock);
	INIT_LIST_HEAD(&params->buffers.queue);

	params->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&params->vdev.entity, 1, &params->pad);
	if (ret)
		goto err_destroy_mutex;

	vb2q->type = V4L2_BUF_TYPE_META_OUTPUT;
	vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
	vb2q->drv_priv = params;
	vb2q->mem_ops = &vb2_dma_contig_memops;
	vb2q->ops = &mali_c55_params_vb2_ops;
	vb2q->buf_struct_size = sizeof(struct mali_c55_buffer);
	vb2q->min_queued_buffers = 1;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2q->lock = &params->lock;
	vb2q->dev = mali_c55->dev;

	ret = vb2_queue_init(vb2q);
	if (ret) {
		dev_err(mali_c55->dev, "params vb2 queue init failed\n");
		goto err_cleanup_entity;
	}

	strscpy(params->vdev.name, "mali-c55 3a params",
		sizeof(params->vdev.name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mali_c55_params_v4l2_fops;
	vdev->ioctl_ops = &mali_c55_params_v4l2_ioctl_ops;
	vdev->lock = &params->lock;
	vdev->v4l2_dev = &mali_c55->v4l2_dev;
	vdev->queue = &params->queue;
	vdev->device_caps = V4L2_CAP_META_OUTPUT | V4L2_CAP_STREAMING;
	vdev->vfl_dir = VFL_DIR_TX;
	video_set_drvdata(vdev, params);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mali_c55->dev,
			"failed to register params video device\n");
		goto err_release_vb2q;
	}

	params->mali_c55 = mali_c55;

	return 0;

err_release_vb2q:
	vb2_queue_release(vb2q);
err_cleanup_entity:
	media_entity_cleanup(&params->vdev.entity);
err_destroy_mutex:
	mutex_destroy(&params->lock);

	return ret;
}
