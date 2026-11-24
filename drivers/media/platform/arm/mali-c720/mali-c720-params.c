// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/byteorder.h>

#include <media/media-entity.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-isp.h>
#include <media/videobuf2-vmalloc.h>

#include "mali-c720-common.h"

/**
 * union mali_c720_params_block - Generalization of a parameter block
 *
 * This union allows the driver to treat a block as a generic pointer to this
 * union and safely access the header and block-specific struct without having
 * to resort to casting. The header member is accessed first, and the
 * header->type field checked which allows the driver to determine which of
 * other members should be used. The data member at the end allows a pointer to
 * an address within the data member of :c:type:`v4l2_isp_params_buffer` to
 * initialize a union variable.
 *
 * @header:		Pointer to the shared header struct embedded as the
 *			first member of all the possible other members (except
 *			@data). This member would be accessed first and the
 *			header->type field checked to determine which of the other
 *			members should be accessed.
 * @digital_gain:	For header->type:
 *			MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP
 * @awb_gains:		For header->type:
 *			MALI_C720_PARAM_BLOCK_AWB_GAINS_HV
 * @black_level:	For header->type:
 *			MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV
 * @square_root:	For header->type:
 *			MALI_C720_PARAM_BLOCK_SQRT_INP
 *			MALI_C720_PARAM_BLOCK_SQRT_HV
 * @square:		For header->type:
 *			MALI_C720_PARAM_BLOCK_SQUARE_A_HV
 *			MALI_C720_PARAM_BLOCK_SQUARE_B_HV
 * @decompander:	For header->type == MALI_C720_PARAM_BLOCK_DECOMPANDER_INP
 * @color_stats:	For header->type:
 *			MALI_C720_PARAM_BLOCK_COLOR_STATS_HV
 * @histogram:		For header->type:
 *			MALI_C720_PARAM_BLOCK_HISTOGRAM_INP
 *			MALI_C720_PARAM_BLOCK_HISTOGRAM_HV
 *			MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX
 * @data:		Allows easy initialization of a union variable with a
 *			pointer into a __u8 array.
 */
union mali_c720_params_block {
	struct v4l2_isp_block_header *header;
	struct mali_c720_params_digital_gain *digital_gain;
	struct mali_c720_params_awb_gains *awb_gains;
	struct mali_c720_params_black_level *black_level;
	struct mali_c720_params_sqrt *square_root;
	struct mali_c720_params_square *square;
	struct mali_c720_params_decompander *decompander;
	struct mali_c720_params_color_stats *color_stats;
	struct mali_c720_params_histogram *histogram;
	__u8 *data;
};

typedef void (*mali_c720_params_handler)(struct mali_c720_context *ctx,
					 const struct mali_c720_param_target *target,
					 union mali_c720_params_block block, bool config);

static const struct mali_c720_discovery_target
mali_c720_params_discovery_targets[MALI_C720_PARAM_BLOCK_MAX] = {
	[MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP] = {
		.module_id = MALI_C720_DISC_MODULE_ID_DIGITAL_GAIN, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_INPUT, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_AWB_GAINS_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_WB_GAIN, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_HV, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_BLACK_LEVEL, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_HV, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_SQRT_INP] = {
		.module_id = MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RAW, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_INPUT, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_SQRT_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_SQUARE_ROOT_RGB, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_HV, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_SQUARE_A_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_SQUARE_RAW, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_HV, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_SQUARE_B_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_SQUARE_RGB, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_HV, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_DECOMPANDER_INP] = {
		.module_id = MALI_C720_DISC_MODULE_ID_DECOMPANDER, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_INPUT, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_COLOR_STATS_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_COLOR_STATS, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_INP] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 0,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 1,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX] = {
		.module_id = MALI_C720_DISC_MODULE_ID_HISTOGRAM_STATS, .module_instance = 2,
		.pipeline_id = MALI_C720_DISC_PIPE_STATS, .pipeline_instance = 0 },
};

static int
mali_c720_params_fill_target(struct mali_c720 *mali_c720, unsigned int block_type,
			     struct mali_c720_param_target *target)
{
	const struct mali_c720_discovery_target *discovery;
	int ret;

	discovery = &mali_c720_params_discovery_targets[block_type];

	if (!mali_c720_discovery_module_present(mali_c720, discovery))
		return -ENOENT;

	ret = mali_c720_discovery_get_module_base(mali_c720, discovery,
						  &target->base);
	if (ret)
		return ret;

	target->present = true;
	return 0;
}

static int mali_c720_params_build_target_cache(struct mali_c720 *mali_c720)
{
	struct mali_c720_params *params = &mali_c720->params;
	unsigned int block_type;
	int ret;

	for (block_type = 0; block_type < MALI_C720_PARAM_BLOCK_MAX; block_type++) {
		ret = mali_c720_params_fill_target(mali_c720, block_type,
						   &params->targets[block_type]);
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;

		params->num_targets++;
	}

	if (!params->num_targets) {
		dev_err(mali_c720->dev, "no configurable parameter targets discovered\n");
		return -ENODEV;
	}

	return 0;
}

static const struct mali_c720_param_target *
mali_c720_params_get_target(struct mali_c720_params *params, unsigned int block_type)
{
	if (block_type >= MALI_C720_PARAM_BLOCK_MAX || !params->targets[block_type].present)
		return NULL;

	return &params->targets[block_type];
}

static int mali_c720_params_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_c720_params *params =
		container_of(ctrl->handler, struct mali_c720_params, ctrls.handler);
	u16 *ids = ctrl->p_new.p_u16;
	unsigned int block_type;

	if (ctrl->id != V4L2_CID_MALI_C720_PARAM_CAPABILITIES)
		return -EINVAL;

	for (block_type = 0; block_type < MALI_C720_PARAM_BLOCK_MAX; block_type++) {
		if (params->targets[block_type].present)
			*ids++ = (u16)block_type;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mali_c720_params_ctrl_ops = {
	.g_volatile_ctrl = mali_c720_params_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config mali_c720_params_caps_ctrl = {
	.ops = &mali_c720_params_ctrl_ops,
	.id = V4L2_CID_MALI_C720_PARAM_CAPABILITIES,
	.name = "Mali-C720 Parameter Capabilities",
	.type = V4L2_CTRL_TYPE_U16,
	.dims = { MALI_C720_PARAM_BLOCK_MAX },
	.min = 0,
	.max = U16_MAX,
	.step = 1,
	.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
};

#define to_mali_c720_params_buf(vbuf) \
	container_of(vbuf, struct mali_c720_params_buf, vb)

static void mali_c720_params_u8_lut_write(struct mali_c720_context *ctx,
					  u32 addr, u8 *lut, u32 size)
{
	unsigned int i, head_elems, tail_elems;
	u32 data;

	/*
	 * Write an array of 8-bit LUT elements to 32-bit registers.
	 * We first write 4 elements at a time until such point that doing so
	 * would exceed the array size, and then write the remaining 1-3 elements.
	 */

	head_elems = size & 0xfffffffc;
	tail_elems = size - head_elems;
	for (i = 0; i < head_elems; i += 4, addr += 4) {
		data = le32_to_cpup((const __le32 *)(lut + i));
		mali_c720_ctx_write_raw(ctx, addr, data);
	}

	data = 0;
	switch (tail_elems) {
	case 3:
		data |= (u32)lut[head_elems + 2] << 16;
		fallthrough;
	case 2:
		data |= (u32)lut[head_elems + 1] << 8;
		fallthrough;
	case 1:
		data |= lut[head_elems];
		mali_c720_ctx_write_raw(ctx, addr, data);
		break;
	default:
		break;
	}
}

static void mali_c720_params_u32_lut_write(struct mali_c720_context *ctx,
					   u32 addr, u32 *lut, u32 size)
{
	unsigned int i;

	for (i = 0; i < size; i++, addr += 4)
		mali_c720_ctx_write_raw(ctx, addr, lut[i]);
}

#define mali_c720_params_updated(params, flag) \
	(!!((params)->updated_mask & BIT_ULL(flag)))

#define mali_c720_ctx_group_write_updated(ctx, group, reg, val, flag)    \
	do {                                                             \
		if (mali_c720_params_updated(params, flag))              \
			mali_c720_ctx_group_write(ctx, group, reg, val); \
	} while (0)

#define mali_c720_lut_u8_write_updated(ctx, addr, lut, size, flag) \
	do { \
		if (mali_c720_params_updated(params, flag)) { \
			mali_c720_params_u8_lut_write(ctx, addr, lut, size); \
		} \
	} while (0)

#define mali_c720_lut_u32_write_updated(ctx, addr, lut, size, flag) \
	do { \
		if (mali_c720_params_updated(params, flag)) { \
			mali_c720_params_u32_lut_write(ctx, addr, lut, size); \
		} \
	} while (0)

static void mali_c720_params_write_bypass(struct mali_c720_context *ctx, u32 base,
					  const struct mali_c720_reg_field *field,
					  const struct v4l2_isp_block_header *header)
{
	u16 flags = header->flags;

	if (flags & (V4L2_ISP_PARAMS_FL_BLOCK_ENABLE | V4L2_ISP_PARAMS_FL_BLOCK_DISABLE))
		mali_c720_ctx_group_write(ctx, base, field,
					  !!(flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE));
}

static void mali_c720_params_input_digital_gain(struct mali_c720_context *ctx,
						const struct mali_c720_param_target *target,
						union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_digital_gain_regs *inp_dgain =
		mali_c720->registers.modules->inp_dgain;
	struct mali_c720_params_digital_gain *params = block.digital_gain;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_inp_dgain, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &inp_dgain->gain,
					  params->gain,
					  MALI_C720_PARAMS_DIGITAL_GAIN_MASK_GAIN);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &inp_dgain->offset,
					  params->offset,
					  MALI_C720_PARAMS_DIGITAL_GAIN_MASK_OFFSET);
}

static void mali_c720_params_awb_gains(struct mali_c720_context *ctx,
				       const struct mali_c720_param_target *target,
				       union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_wb_gain_regs *wb_gain = mali_c720->registers.modules->wb_gain;
	struct mali_c720_params_awb_gains *params = block.awb_gains;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_hv_wb_gain, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &wb_gain->gain_00,
					  params->gain00,
					  MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN00);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &wb_gain->gain_01,
					  params->gain01,
					  MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN01);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &wb_gain->gain_10,
					  params->gain10,
					  MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN10);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &wb_gain->gain_11,
					  params->gain11,
					  MALI_C720_PARAMS_AWB_GAINS_MASK_GAIN11);
}

static void mali_c720_params_black_level(struct mali_c720_context *ctx,
					 const struct mali_c720_param_target *target,
					 union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_black_level_regs *black_level =
		mali_c720->registers.modules->black_level;
	struct mali_c720_params_black_level *params = block.black_level;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_hv_black_level, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &black_level->black_00,
					  params->chan00,
					  MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN00);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &black_level->black_01,
					  params->chan01,
					  MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN01);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &black_level->black_10,
					  params->chan10,
					  MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN10);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &black_level->black_11,
					  params->chan11,
					  MALI_C720_PARAMS_BLACK_LEVEL_MASK_CHAN11);
}

static void mali_c720_params_sqrt_a(struct mali_c720_context *ctx,
				    const struct mali_c720_param_target *target,
				    union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_square_root_regs *square_root_a =
		mali_c720->registers.modules->square_root_a;
	struct mali_c720_params_sqrt *params = block.square_root;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_inp_square_root_a, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_a->black_level_in,
					  params->black_level_linear,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_IN);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_a->black_level_out,
					  params->black_level_sqrt,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_OUT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_a->normalization,
					  params->normalization,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_NORMALIZATION);
}

static void mali_c720_params_sqrt_b(struct mali_c720_context *ctx,
				    const struct mali_c720_param_target *target,
				    union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_square_root_regs *square_root_b =
		mali_c720->registers.modules->square_root_b;
	struct mali_c720_params_sqrt *params = block.square_root;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_hv_square_root_b, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_b->black_level_in,
					  params->black_level_linear,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_IN);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_b->black_level_out,
					  params->black_level_sqrt,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_OUT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_root_b->normalization,
					  params->normalization,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_NORMALIZATION);
}

static void mali_c720_params_square_a(struct mali_c720_context *ctx,
				      const struct mali_c720_param_target *target,
				      union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_square_regs *square_a = mali_c720->registers.modules->square_a;
	struct mali_c720_params_square *params = block.square;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_hv_square_a, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_a->black_level_in,
					  params->black_level_sqrt,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_IN);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_a->black_level_out,
					  params->black_level_linear,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_OUT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_a->normalization,
					  params->normalization,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_NORMALIZATION);
}

static void mali_c720_params_square_b(struct mali_c720_context *ctx,
				      const struct mali_c720_param_target *target,
				      union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_square_regs *square_b = mali_c720->registers.modules->square_b;
	struct mali_c720_params_square *params = block.square;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_hv_square_b, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_b->black_level_in,
					  params->black_level_sqrt,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_IN);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_b->black_level_out,
					  params->black_level_linear,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_BL_OUT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &square_b->normalization,
					  params->normalization,
					  MALI_C720_PARAMS_SQUARE_SQRT_MASK_NORMALIZATION);
}

static int
mali_c720_params_validate_decompander(struct device *dev,
				      const struct v4l2_isp_block_header *block)
{
	const struct mali_c720 *mali_c720 = dev_get_drvdata(dev);
	const struct mali_c720_decompander_regs *decompander =
		mali_c720->registers.modules->decompander;
	const struct mali_c720_params_decompander *params =
		(const struct mali_c720_params_decompander *)block;
	const u64 lut_mask = BIT_ULL(MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_X) |
			     BIT_ULL(MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_Y);
	u64 updated;
	unsigned int i;

	if (block->size == sizeof(*block))
		return 0;

	updated = params->updated_mask & lut_mask;
	if (!updated)
		return 0;

	if (updated != lut_mask) {
		dev_dbg(dev, "Decompander x/y LUTs must be updated together\n");
		return -EINVAL;
	}

	/*
	 * Reject decreasing X coordinates.
	 * Increasing X coordinates must differ by at least 8.
	 * Equal X coordinates must have equal Y coordinates.
	 */
	for (i = 1; i < ARRAY_SIZE(params->lut_x); i++) {
		u32 prev_x = params->lut_x[i - 1] & mali_c720_reg_mask(&decompander->lut_x);
		u32 prev_y = params->lut_y[i - 1] & mali_c720_reg_mask(&decompander->lut_y);
		u32 x = params->lut_x[i] & mali_c720_reg_mask(&decompander->lut_x);
		u32 y = params->lut_y[i] & mali_c720_reg_mask(&decompander->lut_y);

		if (x < prev_x || (x != prev_x && x - prev_x < 8) ||
		    (x == prev_x && y != prev_y)) {
			dev_dbg(dev, "Invalid decompander LUT interval at index %u\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

static void mali_c720_params_decompander(struct mali_c720_context *ctx,
					 const struct mali_c720_param_target *target,
					 union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_decompander_regs *decompander =
		mali_c720->registers.modules->decompander;
	struct mali_c720_params_decompander *params = block.decompander;
	u32 lut_addr;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->bypass_inp_decompander, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &decompander->enable,
					  params->enable,
					  MALI_C720_PARAMS_DECOMPANDER_MASK_ENABLE);

	lut_addr = mali_c720_reg_addr(target->base, &decompander->lut_x);
	mali_c720_lut_u32_write_updated(ctx, lut_addr,
					params->lut_x,
					ARRAY_SIZE(params->lut_x),
					MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_X);

	lut_addr = mali_c720_reg_addr(target->base, &decompander->lut_y);
	mali_c720_lut_u32_write_updated(ctx, lut_addr,
					params->lut_y,
					ARRAY_SIZE(params->lut_y),
					MALI_C720_PARAMS_DECOMPANDER_MASK_LUT_Y);
}

static int
mali_c720_params_validate_color_stats(struct device *dev,
				      const struct v4l2_isp_block_header *block)
{
	const struct mali_c720_params_color_stats *params =
		(const struct mali_c720_params_color_stats *)block;
	const u64 zone_mask = BIT_ULL(MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_HORIZ) |
			      BIT_ULL(MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_VERT);
	u64 updated;

	if (block->size == sizeof(*block))
		return 0;

	updated = params->updated_mask & zone_mask;
	if (!updated)
		return 0;

	if (updated != zone_mask) {
		dev_dbg(dev, "Color stats zone dimensions must be updated together\n");
		return -EINVAL;
	}

	/*
	 * Horizontal zone count must be between 3 and 32.
	 * Vertical zone count must be between 3 and 255.
	 * The total number of zones must not exceed MALI_C720_MAX_ZONES.
	 */
	if (params->nodes_used_horiz < 3 || params->nodes_used_horiz > 32 ||
	    params->nodes_used_vert < 3 ||
	    params->nodes_used_horiz * params->nodes_used_vert > MALI_C720_MAX_ZONES) {
		dev_dbg(dev, "Invalid color statistics zone dimensions\n");
		return -EINVAL;
	}

	return 0;
}

static void mali_c720_params_color_stats(struct mali_c720_context *ctx,
					 const struct mali_c720_param_target *target,
					 union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_stat_color_stats_regs *stat_color_stats =
		mali_c720->registers.modules->stat_color_stats;
	struct mali_c720_params_color_stats *params = block.color_stats;
	u32 weight_addr;

	mali_c720_params_write_bypass(ctx, mali_c720->top_base,
				      &pipeline->color_stats_0_mux_sel, block.header);

	if (!config || !params->updated_mask)
		return;

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->awb_nodes_used_horiz,
					  params->nodes_used_horiz,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_HORIZ);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->awb_nodes_used_vert,
					  params->nodes_used_vert,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_NODES_USED_VERT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->awb_stats_mode,
					  params->awb_stats_mode,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_AWB_STATS_MODE);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->white_level_awb,
					  params->white_level_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_WHITE_LEVEL_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->black_level_awb,
					  params->black_level_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_BLACK_LEVEL_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cr_ref_max_awb,
					  params->cr_ref_max_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_MAX_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cr_ref_min_awb,
					  params->cr_ref_min_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_MIN_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cr_ref_high_awb,
					  params->cr_ref_high_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_HIGH_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cr_ref_low_awb,
					  params->cr_ref_low_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CR_REF_LOW_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cb_ref_max_awb,
					  params->cb_ref_max_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_MAX_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cb_ref_min_awb,
					  params->cb_ref_min_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_MIN_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cb_ref_high_awb,
					  params->cb_ref_high_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_HIGH_AWB);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_color_stats->cb_ref_low_awb,
					  params->cb_ref_low_awb,
					  MALI_C720_PARAMS_COLOR_STATS_MASK_CB_REF_LOW_AWB);

	weight_addr = mali_c720_reg_addr(target->base, &stat_color_stats->awb_weight);
	mali_c720_lut_u8_write_updated(ctx, weight_addr,
				       params->weights,
				       ARRAY_SIZE(params->weights),
				       MALI_C720_PARAMS_COLOR_STATS_MASK_WEIGHTS);
}

static int
mali_c720_params_validate_histogram(struct device *dev,
				    const struct v4l2_isp_block_header *block)
{
	const struct mali_c720_params_histogram *params =
		(const struct mali_c720_params_histogram *)block;
	const u64 zone_mask = BIT_ULL(MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_HORIZ) |
			      BIT_ULL(MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_VERT);
	u64 updated;

	if (block->size == sizeof(*block))
		return 0;

	updated = params->updated_mask & zone_mask;
	if (updated && updated != zone_mask) {
		dev_dbg(dev, "Histogram zone dimensions must be updated together\n");
		return -EINVAL;
	}

	if (updated && (!params->nodes_used_horiz || !params->nodes_used_vert ||
			params->nodes_used_horiz * params->nodes_used_vert >
			MALI_C720_MAX_ZONES)) {
		dev_dbg(dev, "Invalid histogram zone dimensions\n");
		return -EINVAL;
	}

	if (mali_c720_params_updated(params, MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_X) &&
	    params->skip_x > MALI_C720_HISTOGRAM_SKIP_X_EVERY_9TH) {
		dev_dbg(dev, "Invalid histogram horizontal skip selection\n");
		return -EINVAL;
	}

	if (mali_c720_params_updated(params, MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_Y) &&
	    params->skip_y > MALI_C720_HISTOGRAM_SKIP_Y_EVERY_9TH) {
		dev_dbg(dev, "Invalid histogram vertical skip selection\n");
		return -EINVAL;
	}

	if (mali_c720_params_updated(params, MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_BOTTOM) &&
	    params->scale_bottom > MALI_C720_HISTOGRAM_SCALE_16X) {
		dev_dbg(dev, "Invalid histogram bottom scale selection\n");
		return -EINVAL;
	}

	if (mali_c720_params_updated(params, MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_TOP) &&
	    params->scale_top > MALI_C720_HISTOGRAM_SCALE_16X) {
		dev_dbg(dev, "Invalid histogram top scale selection\n");
		return -EINVAL;
	}

	if (mali_c720_params_updated(params, MALI_C720_PARAMS_HISTOGRAM_MASK_PLANE_MODE) &&
	    (params->plane_mode == 3 || params->plane_mode > MALI_C720_HISTOGRAM_FOCUS_11)) {
		dev_dbg(dev, "Invalid histogram plane mode\n");
		return -EINVAL;
	}

	return 0;
}

static void mali_c720_params_histogram(struct mali_c720_context *ctx,
				       const struct mali_c720_param_target *target,
				       union mali_c720_params_block block, bool config)
{
	struct mali_c720 *mali_c720 = ctx->mali_c720;
	const struct mali_c720_pipeline_regs *pipeline = mali_c720->registers.top->pipeline;
	const struct mali_c720_stat_histogram_regs *stat_histogram =
		mali_c720->registers.modules->stat_histogram;
	struct mali_c720_params_histogram *params = block.histogram;
	u32 weight_addr;

	switch (block.header->type) {
	case MALI_C720_PARAM_BLOCK_HISTOGRAM_INP:
		mali_c720_params_write_bypass(ctx, mali_c720->top_base,
					      &pipeline->histogram_0_mux_sel, block.header);
		break;
	case MALI_C720_PARAM_BLOCK_HISTOGRAM_HV:
		mali_c720_params_write_bypass(ctx, mali_c720->top_base,
					      &pipeline->histogram_1_mux_sel, block.header);
		break;
	case MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX:
		mali_c720_params_write_bypass(ctx, mali_c720->top_base,
					      &pipeline->histogram_2_mux_sel, block.header);
		break;
	default:
		return;
	}

	if (!config || !params->updated_mask)
		return;

	if (block.header->type == MALI_C720_PARAM_BLOCK_HISTOGRAM_HV) {
		mali_c720_ctx_group_write_updated(ctx, mali_c720->top_base,
						  &pipeline->histogram_1_stream_mux_sel,
						  params->stream_mux_select,
						  MALI_C720_PARAMS_HISTOGRAM_MASK_STREAM_MUX_SEL);
	}

	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->nodes_used_horiz,
					  params->nodes_used_horiz,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_HORIZ);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->nodes_used_vert,
					  params->nodes_used_vert,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_NODES_USED_VERT);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->skip_x,
					  params->skip_x,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_X);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->skip_y,
					  params->skip_y,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_SKIP_Y);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->offset_x,
					  params->offset_x,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_OFFSET_X);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->offset_y,
					  params->offset_y,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_OFFSET_Y);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->scale_bottom,
					  params->scale_bottom,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_BOTTOM);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->scale_top,
					  params->scale_top,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_SCALE_TOP);
	mali_c720_ctx_group_write_updated(ctx, target->base,
					  &stat_histogram->plane_mode,
					  params->plane_mode,
					  MALI_C720_PARAMS_HISTOGRAM_MASK_PLANE_MODE);

	weight_addr = mali_c720_reg_addr(target->base, &stat_histogram->weight);
	mali_c720_lut_u8_write_updated(ctx, weight_addr,
				       params->weights,
				       ARRAY_SIZE(params->weights),
				       MALI_C720_PARAMS_HISTOGRAM_MASK_WEIGHTS);
}

static const mali_c720_params_handler mali_c720_params_handlers[] = {
	[MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP] = &mali_c720_params_input_digital_gain,
	[MALI_C720_PARAM_BLOCK_AWB_GAINS_HV] = &mali_c720_params_awb_gains,
	[MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV] = &mali_c720_params_black_level,
	[MALI_C720_PARAM_BLOCK_SQRT_INP] = &mali_c720_params_sqrt_a,
	[MALI_C720_PARAM_BLOCK_SQRT_HV] = &mali_c720_params_sqrt_b,
	[MALI_C720_PARAM_BLOCK_SQUARE_A_HV] = &mali_c720_params_square_a,
	[MALI_C720_PARAM_BLOCK_SQUARE_B_HV] = &mali_c720_params_square_b,
	[MALI_C720_PARAM_BLOCK_DECOMPANDER_INP] = &mali_c720_params_decompander,
	[MALI_C720_PARAM_BLOCK_COLOR_STATS_HV] = &mali_c720_params_color_stats,
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_INP] = &mali_c720_params_histogram,
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV] = &mali_c720_params_histogram,
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX] = &mali_c720_params_histogram,
};

static const struct v4l2_isp_params_block_type_info
mali_c720_params_block_types_info[MALI_C720_PARAM_BLOCK_MAX] = {
	[MALI_C720_PARAM_BLOCK_DIGITAL_GAIN_INP] = {
		.size = sizeof(struct mali_c720_params_digital_gain),
	},
	[MALI_C720_PARAM_BLOCK_AWB_GAINS_HV] = {
		.size = sizeof(struct mali_c720_params_awb_gains),
	},
	[MALI_C720_PARAM_BLOCK_BLACK_LEVEL_HV] = {
		.size = sizeof(struct mali_c720_params_black_level),
	},
	[MALI_C720_PARAM_BLOCK_SQRT_INP] = {
		.size = sizeof(struct mali_c720_params_sqrt),
	},
	[MALI_C720_PARAM_BLOCK_SQRT_HV] = {
		.size = sizeof(struct mali_c720_params_sqrt),
	},
	[MALI_C720_PARAM_BLOCK_SQUARE_A_HV] = {
		.size = sizeof(struct mali_c720_params_square),
	},
	[MALI_C720_PARAM_BLOCK_SQUARE_B_HV] = {
		.size = sizeof(struct mali_c720_params_square),
	},
	[MALI_C720_PARAM_BLOCK_DECOMPANDER_INP] = {
		.size = sizeof(struct mali_c720_params_decompander),
		.block_validate = mali_c720_params_validate_decompander,
	},
	[MALI_C720_PARAM_BLOCK_COLOR_STATS_HV] = {
		.size = sizeof(struct mali_c720_params_color_stats),
		.block_validate = mali_c720_params_validate_color_stats,
	},
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_INP] = {
		.size = sizeof(struct mali_c720_params_histogram),
		.block_validate = mali_c720_params_validate_histogram,
	},
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV] = {
		.size = sizeof(struct mali_c720_params_histogram),
		.block_validate = mali_c720_params_validate_histogram,
	},
	[MALI_C720_PARAM_BLOCK_HISTOGRAM_HV_IRIDIX] = {
		.size = sizeof(struct mali_c720_params_histogram),
		.block_validate = mali_c720_params_validate_histogram,
	},
};

static_assert(ARRAY_SIZE(mali_c720_params_handlers) ==
	      ARRAY_SIZE(mali_c720_params_block_types_info));

static int
mali_c720_params_validate_block(struct mali_c720 *mali_c720,
				union mali_c720_params_block block)
{
	const struct mali_c720_param_target *target;

	target = mali_c720_params_get_target(&mali_c720->params, block.header->type);
	if (!target) {
		dev_dbg(mali_c720->dev,
			"Parameters block targets unavailable module\n");
		return -EINVAL;
	}

	return 0;
}

void mali_c720_params_apply_config(struct mali_c720 *mali_c720,
				   struct mali_c720_params_buf *buf)
{
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_params *params = &mali_c720->params;
	struct v4l2_isp_params_buffer *buffer;
	size_t block_offset = 0;
	size_t max_offset;

	buffer = buf->config;
	max_offset = buffer->data_size;

	/*
	 * Walk the list of parameter blocks and process them. No validation is
	 * done here, as the contents of the config buffer are already checked
	 * in .buf_prepare().
	 */
	while (block_offset < max_offset) {
		mali_c720_params_handler handler;
		const struct mali_c720_param_target *target;
		union mali_c720_params_block block = {
			.data = &buffer->data[block_offset],
		};
		bool config = true;

		target = mali_c720_params_get_target(params, block.header->type);

		/* We checked the array index already in .buf_prepare() */
		handler = mali_c720_params_handlers[block.header->type];
		if (!target) {
			dev_warn_once(mali_c720->dev,
				      "params target unexpectedly unavailable during write\n");
			block_offset += block.header->size;
			continue;
		}

		if ((block.header->flags & V4L2_ISP_PARAMS_FL_BLOCK_DISABLE) &&
		    block.header->size == sizeof(*block.header))
			config = false;

		handler(ctx, target, block, config);

		block_offset += block.header->size;
	}
}

static int mali_c720_params_enum_fmt_meta_out(struct file *file, void *fh,
					      struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	if (f->mbus_code && f->mbus_code != MEDIA_BUS_FMT_METADATA_FIXED)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_MALI_C720_PARAMS;

	return 0;
}

static int mali_c720_params_g_fmt_meta_out(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	static const struct v4l2_meta_format mfmt = {
		.dataformat = V4L2_META_FMT_MALI_C720_PARAMS,
		.buffersize = v4l2_isp_buffer_size(MALI_C720_PARAMS_MAX_SIZE),
	};

	f->fmt.meta = mfmt;

	return 0;
}

static int mali_c720_params_querycap(struct file *file,
				     void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, MALI_C720_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ARM Mali-C720 ISP", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops mali_c720_params_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = mali_c720_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_out = mali_c720_params_enum_fmt_meta_out,
	.vidioc_g_fmt_meta_out = mali_c720_params_g_fmt_meta_out,
	.vidioc_s_fmt_meta_out = mali_c720_params_g_fmt_meta_out,
	.vidioc_try_fmt_meta_out = mali_c720_params_g_fmt_meta_out,
	.vidioc_querycap = mali_c720_params_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations mali_c720_params_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static int
mali_c720_params_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			     unsigned int *num_planes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	if (*num_planes && *num_planes > 1)
		return -EINVAL;

	if (sizes[0] && sizes[0] <
	    v4l2_isp_buffer_size(MALI_C720_PARAMS_MAX_SIZE))
		return -EINVAL;

	*num_planes = 1;

	if (!sizes[0])
		sizes[0] = v4l2_isp_buffer_size(MALI_C720_PARAMS_MAX_SIZE);

	return 0;
}

static int mali_c720_params_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_params_buf *buf = to_mali_c720_params_buf(vbuf);

	buf->config = kvmalloc(v4l2_isp_buffer_size(MALI_C720_PARAMS_MAX_SIZE),
			       GFP_KERNEL);
	if (!buf->config)
		return -ENOMEM;

	return 0;
}

static void mali_c720_params_buf_cleanup(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_params_buf *buf = to_mali_c720_params_buf(vbuf);

	kvfree(buf->config);
	buf->config = NULL;
}

static int mali_c720_params_buf_prepare(struct vb2_buffer *vb)
{
	struct mali_c720_params *params = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_params_buf *buf = to_mali_c720_params_buf(vbuf);
	struct mali_c720 *mali_c720 = params->mali_c720;
	struct v4l2_isp_params_buffer *config = vb2_plane_vaddr(vb, 0);
	struct dma_buf *dbuf = vb->planes[0].dbuf;
	size_t block_offset = 0;
	int ret;

	ret = v4l2_isp_params_validate_buffer_size(mali_c720->dev,
						   vb,
						   v4l2_isp_buffer_size
						   (MALI_C720_PARAMS_MAX_SIZE));
	if (ret)
		return ret;

	if (dbuf) {
		ret = dma_buf_begin_cpu_access(dbuf, DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	/*
	 * Copy the parameters buffer provided by userspace to the internal
	 * scratch buffer. This protects against the chance of userspace making
	 * changes to the buffer content whilst the driver processes it.
	 */
	memcpy(buf->config, config,
	       v4l2_isp_buffer_size(MALI_C720_PARAMS_MAX_SIZE));

	if (dbuf) {
		ret = dma_buf_end_cpu_access(dbuf, DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	ret = v4l2_isp_params_validate_buffer(mali_c720->dev, vb, buf->config,
					      mali_c720_params_block_types_info,
					      ARRAY_SIZE(mali_c720_params_block_types_info));
	if (ret)
		return ret;

	while (block_offset < buf->config->data_size) {
		union mali_c720_params_block block = {
			.data = &buf->config->data[block_offset],
		};

		ret = mali_c720_params_validate_block(mali_c720, block);
		if (ret)
			return ret;

		block_offset += block.header->size;
	}

	return 0;
}

static void mali_c720_params_buf_queue(struct vb2_buffer *vb)
{
	struct mali_c720_params *params = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c720_params_buf *buf = to_mali_c720_params_buf(vbuf);
	struct mali_c720 *mali_c720 = params->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);

	scoped_guard(spinlock, &params->buffers.lock)
		list_add_tail(&buf->queue, &params->buffers.input);

	if (!mali_c720_params_uses_requests(params))
		mali_c720_try_schedule_frame(ctx);
}

static void mali_c720_buf_request_complete(struct vb2_buffer *vb)
{
}

bool mali_c720_params_uses_requests(struct mali_c720_params *params)
{
	return video_is_registered(&params->vdev) &&
	       vb2_is_streaming(&params->queue) &&
	       params->queue.uses_requests;
}

struct media_request *mali_c720_params_get_request(struct mali_c720_params *params)
{
	struct mali_c720_params_buf *buf;

	if (!mali_c720_params_uses_requests(params))
		return NULL;

	guard(spinlock)(&params->buffers.lock);

	buf = list_first_entry_or_null(&params->buffers.input,
				       struct mali_c720_params_buf, queue);

	return buf ? buf->vb.vb2_buf.request : NULL;
}

struct mali_c720_params_buf *
mali_c720_params_get_next_buffer(struct mali_c720_params *params,
				 struct media_request *req)
{
	struct mali_c720_params_buf *buf;

	if (!video_is_registered(&params->vdev))
		return NULL;

	if (!vb2_is_streaming(&params->queue))
		return NULL;

	scoped_guard(spinlock, &params->buffers.lock) {
		buf = list_first_entry_or_null(&params->buffers.input,
					       struct mali_c720_params_buf, queue);
		if (!buf || (buf->vb.vb2_buf.request &&
			     buf->vb.vb2_buf.request != req))
			return NULL;

		list_move_tail(&buf->queue, &params->buffers.processing);
	}

	return buf;
}

void mali_c720_params_buffer_done(struct mali_c720_params *params, bool success)
{
	struct mali_c720 *mali_c720 = params->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_params_buf *buf;

	if (!video_is_registered(&params->vdev))
		return;

	if (!vb2_is_streaming(&params->queue))
		return;

	scoped_guard(spinlock, &params->buffers.lock) {
		buf = list_first_entry_or_null(&params->buffers.processing,
					       struct mali_c720_params_buf, queue);
		if (!buf)
			return;

		if (success) {
			buf->vb.vb2_buf.timestamp = ctx->timestamp;
			buf->vb.sequence = ctx->frame_sequence;
		}

		vb2_buffer_done(&buf->vb.vb2_buf,
				success ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
		list_del(&buf->queue);
	}
}

static void mali_c720_params_return_buffers(struct mali_c720_params *params,
					    enum vb2_buffer_state state)
{
	struct mali_c720_params_buf *buf, *tmp;

	guard(spinlock) (&params->buffers.lock);

	list_for_each_entry_safe(buf, tmp, &params->buffers.input, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	list_for_each_entry_safe(buf, tmp, &params->buffers.processing, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static int mali_c720_params_start_streaming(struct vb2_queue *q,
					    unsigned int count)
{
	struct mali_c720_params *params = vb2_get_drv_priv(q);
	struct mali_c720 *mali_c720 = params->mali_c720;
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

	ret = video_device_pipeline_start(&params->vdev, &mali_c720->pipe);
	if (ret)
		goto err_pm_put;

	ret = mali_c720_alloc_context(ctx);
	if (ret)
		goto err_stop_video_pipeline;

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
err_stop_video_pipeline:
	video_device_pipeline_stop(&params->vdev);
err_pm_put:
	pm_runtime_put(mali_c720->dev);
err_return_buffers:
	mali_c720_params_return_buffers(params, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void mali_c720_params_stop_streaming(struct vb2_queue *q)
{
	struct mali_c720_params *params = vb2_get_drv_priv(q);
	struct mali_c720 *mali_c720 = params->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct v4l2_subdev *isp_sd = &isp->mdev.sd;

	guard(mutex)(&mali_c720->device_lock);

	v4l2_subdev_disable_streams(isp_sd, MALI_C720_ISP_PAD_SOURCE_HV,
				    BIT(0));
	mali_c720_free_context(ctx);

	video_device_pipeline_stop(&params->vdev);
	mali_c720_params_return_buffers(params, VB2_BUF_STATE_ERROR);

	pm_runtime_put_autosuspend(mali_c720->dev);
}

static const struct vb2_ops mali_c720_params_vb2_ops = {
	.queue_setup = mali_c720_params_queue_setup,
	.buf_init = mali_c720_params_buf_init,
	.buf_cleanup = mali_c720_params_buf_cleanup,
	.buf_prepare = mali_c720_params_buf_prepare,
	.buf_queue = mali_c720_params_buf_queue,
	.buf_request_complete = mali_c720_buf_request_complete,
	.start_streaming = mali_c720_params_start_streaming,
	.stop_streaming = mali_c720_params_stop_streaming,
};

void mali_c720_unregister_params(struct mali_c720 *mali_c720)
{
	struct mali_c720_params *params = &mali_c720->params;

	if (!video_is_registered(&params->vdev))
		return;

	vb2_video_unregister_device(&params->vdev);
}

void mali_c720_cleanup_params(struct mali_c720 *mali_c720)
{
	struct mali_c720_params *params = &mali_c720->params;

	if (!params->mali_c720)
		return;

	v4l2_ctrl_handler_free(&params->ctrls.handler);
	media_entity_cleanup(&params->vdev.entity);
	mutex_destroy(&params->lock);
	params->mali_c720 = NULL;
}

int mali_c720_register_params(struct mali_c720 *mali_c720)
{
	struct mali_c720_params *params = &mali_c720->params;
	struct video_device *vdev = &params->vdev;
	struct vb2_queue *vb2q = &params->queue;
	u32 dims[V4L2_CTRL_MAX_DIMS] = { 0 };
	int ret;

	params->mali_c720 = mali_c720;
	mutex_init(&params->lock);
	ret = mali_c720_params_build_target_cache(mali_c720);
	if (ret) {
		dev_err(mali_c720->dev,
			"failed to build params target cache\n");
		goto err_destroy_mutex;
	}

	INIT_LIST_HEAD(&params->buffers.input);
	INIT_LIST_HEAD(&params->buffers.processing);
	spin_lock_init(&params->buffers.lock);

	ret = v4l2_ctrl_handler_init(&params->ctrls.handler, 1);
	if (ret)
		goto err_destroy_mutex;

	params->ctrls.param_capabilities =
		v4l2_ctrl_new_custom(&params->ctrls.handler,
				     &mali_c720_params_caps_ctrl, NULL);
	if (params->ctrls.param_capabilities) {
		dims[0] = params->num_targets;
		ret = v4l2_ctrl_modify_dimensions(params->ctrls.param_capabilities,
						  dims);
		if (ret)
			goto err_free_ctrl_handler;
	}

	if (params->ctrls.handler.error) {
		ret = params->ctrls.handler.error;
		goto err_free_ctrl_handler;
	}

	params->pad.flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&params->vdev.entity, 1, &params->pad);
	if (ret)
		goto err_free_ctrl_handler;

	vb2q->type = V4L2_BUF_TYPE_META_OUTPUT;
	vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
	vb2q->drv_priv = params;
	vb2q->mem_ops = &vb2_vmalloc_memops;
	vb2q->ops = &mali_c720_params_vb2_ops;
	vb2q->buf_struct_size = sizeof(struct mali_c720_params_buf);
	vb2q->min_queued_buffers = 0;
	vb2q->supports_requests = 1;
	vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb2q->lock = &params->lock;
	vb2q->dev = mali_c720->dev;

	ret = vb2_queue_init(vb2q);
	if (ret) {
		dev_err(mali_c720->dev, "params vb2 queue init failed\n");
		goto err_cleanup_entity;
	}

	strscpy(params->vdev.name, "mali-c720 3a params",
		sizeof(params->vdev.name));
	vdev->release = video_device_release_empty;
	vdev->fops = &mali_c720_params_v4l2_fops;
	vdev->ioctl_ops = &mali_c720_params_v4l2_ioctl_ops;
	vdev->lock = &params->lock;
	vdev->v4l2_dev = &mali_c720->v4l2_dev;
	vdev->queue = &params->queue;
	vdev->ctrl_handler = &params->ctrls.handler;
	vdev->device_caps = V4L2_CAP_META_OUTPUT |
			    V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->vfl_dir = VFL_DIR_TX;
	video_set_drvdata(vdev, params);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(mali_c720->dev,
			"failed to register params video device\n");
		goto err_release_vb2q;
	}

	return 0;

err_release_vb2q:
	vb2_queue_release(vb2q);
err_cleanup_entity:
	media_entity_cleanup(&params->vdev.entity);
err_free_ctrl_handler:
	v4l2_ctrl_handler_free(&params->ctrls.handler);
err_destroy_mutex:
	mutex_destroy(&params->lock);
	params->mali_c720 = NULL;

	return ret;
}
