// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>

#include "mali-c720-common.h"

#define MALI_C720_TPG_SRC_PAD			0
#define MALI_C720_TPG_DEFAULT_STREAM		0
#define MALI_C720_TPG_FIXED_HBLANK		0x20
#define MALI_C720_TPG_MAX_VBLANK_CYCLES		0xfffffff

static const char *const mali_c720_tpg_test_pattern_menu[] = {
	"Flat field",
	"Horizontal gradient",
	"Vertical gradient",
	"Vertical bars",
	"Arbitrary rectangle",
	"Random data",
	"White frame on black field",
};

static void mali_c720_tpg_start(struct mali_c720_tpg *tpg)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	const struct mali_c720_precm_tpg_regs *precm_tpgs = mali_c720->registers.top->precm_tpgs;
	u32 base = mali_c720_group_base(&precm_tpgs->group, tpg->id);

	mali_c720_group_write(mali_c720, base, &precm_tpgs->test_pattern_off_on, 0x01);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->generate_mode, 0x01);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->frame_request, 0x01);
}

static void mali_c720_tpg_stop(struct mali_c720_tpg *tpg)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	const struct mali_c720_precm_tpg_regs *precm_tpgs = mali_c720->registers.top->precm_tpgs;
	u32 base = mali_c720_group_base(&precm_tpgs->group, tpg->id);

	mali_c720_group_write(mali_c720, base, &precm_tpgs->test_pattern_off_on, 0x00);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->generate_mode, 0x00);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->frame_request, 0x00);
}

static void mali_c720_tpg_program_vertical_blank(struct mali_c720_tpg *tpg,
						 struct v4l2_subdev_state *state)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	const struct mali_c720_precm_tpg_regs *precm_tpgs = mali_c720->registers.top->precm_tpgs;
	struct v4l2_mbus_framefmt *format;
	u32 vblank_cycles;
	u32 base = mali_c720_group_base(&precm_tpgs->group, tpg->id);

	format = v4l2_subdev_state_get_format(state, MALI_C720_TPG_SRC_PAD,
					      MALI_C720_TPG_DEFAULT_STREAM);
	vblank_cycles = (u32)tpg->ctrls.vblank->cur.val *
			(MALI_C720_TPG_FIXED_HBLANK + format->width);

	mali_c720_group_write(mali_c720, base, &precm_tpgs->vertical_blanking, vblank_cycles);
}

static void mali_c720_tpg_program_test_pattern(struct mali_c720_tpg *tpg,
					       u32 pattern)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	const struct mali_c720_precm_tpg_regs *precm_tpgs = mali_c720->registers.top->precm_tpgs;
	u32 base = mali_c720_group_base(&precm_tpgs->group, tpg->id);

	mali_c720_group_write(mali_c720, base, &precm_tpgs->pattern_type, pattern);
}

static void mali_c720_tpg_calculate_vblank(struct mali_c720_tpg *tpg,
					   unsigned int width, unsigned int height,
					   unsigned int *min_vblank,
					   unsigned int *max_vblank,
					   unsigned int *def_vblank)
{
	const unsigned long tpg_clk_rate = tpg->clk_rate;
	unsigned int hts;
	unsigned long tgt_fps;

	hts = width + MALI_C720_TPG_FIXED_HBLANK;

	/*
	 * The ISP minimum vertical blanking depends on the worst case pipeline
	 * latency, which partially overlaps with Iridix filtering. Convert the
	 * filtering time from ISP clock cycles to lines and allow one
	 * additional line for final DMA completion.
	 */
	*min_vblank = max(31, 21 + DIV_ROUND_UP(60350U, hts)) + 1;

	/*
	 * A high clock rate combined with a small frame and low frame rate can
	 * exceed the TPG vertical blanking limit.
	 */
	*max_vblank = MALI_C720_TPG_MAX_VBLANK_CYCLES / hts;

	/*
	 * Target a multiple of 15 frames per second, falling back to 5 frames
	 * per second and then to the minimum vertical blanking.
	 */
	tgt_fps = tpg_clk_rate / hts / (height + *min_vblank);

	if (tgt_fps < 5)
		*def_vblank = *min_vblank;
	else
		*def_vblank = (unsigned int)((tpg_clk_rate / hts
			   / max(rounddown(tgt_fps, 15), 5)) - height);

	*def_vblank = clamp(*def_vblank, *min_vblank, *max_vblank);
}

static int mali_c720_tpg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_c720_tpg *tpg = container_of(ctrl->handler,
						 struct mali_c720_tpg,
						 ctrls.handler);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		if (v4l2_subdev_is_streaming(&tpg->mdev.sd))
			mali_c720_tpg_program_test_pattern(tpg, (u32)ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		if (v4l2_subdev_is_streaming(&tpg->mdev.sd))
			ret = -EBUSY;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops mali_c720_tpg_ctrl_ops = {
	.s_ctrl = &mali_c720_tpg_s_ctrl,
};

static int mali_c720_tpg_configure(struct mali_c720_tpg *tpg,
				   struct v4l2_subdev_state *state)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	const struct mali_c720_precm_tpg_regs *precm_tpgs = mali_c720->registers.top->precm_tpgs;
	const struct mali_c720_pre_cm_regs *pre_cm = mali_c720->registers.top->pre_cm;
	const struct mali_c720_mbus_format_info *cfg;
	struct v4l2_mbus_framefmt *fmt;
	u32 base = mali_c720_group_base(&precm_tpgs->group, tpg->id);
	u32 bayer_order;

	fmt = v4l2_subdev_state_get_format(state,
					   MALI_C720_TPG_SRC_PAD,
					   MALI_C720_TPG_DEFAULT_STREAM);

	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg)
		return -EINVAL;

	bayer_order = __ffs(cfg->type);

	tpg->remote = media_pad_remote_pad_unique(&tpg->pad);
	if (IS_ERR(tpg->remote))
		return (int)PTR_ERR(tpg->remote);

	if (tpg->remote->index != MALI_C720_INPUT_PAD_SINK_INPUT0)
		return -EINVAL;

	/*
	 * hblank needs setting, but is a read-only control and thus won't be
	 * called during __v4l2_ctrl_handler_setup(). Do it here instead.
	 */
	mali_c720_group_write(mali_c720, base, &precm_tpgs->horizontal_blanking,
			      MALI_C720_TPG_FIXED_HBLANK);

	if (tpg->id == MALI_C720_TPG0)
		mali_c720_write(mali_c720,
				&pre_cm->tpg_0_splitter_sel,
				0);
	else
		mali_c720_write(mali_c720,
				&pre_cm->tpg_1_splitter_sel,
				0);

	mali_c720_group_write(mali_c720, base, &precm_tpgs->glbl_rggb_start, bayer_order);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->glbl_active_width, fmt->width);
	mali_c720_group_write(mali_c720, base, &precm_tpgs->glbl_active_height, fmt->height);

	mali_c720_write(mali_c720, &pre_cm->input_0_mux_sel,
			tpg->id == MALI_C720_TPG0 ? 1 : 2);

	return 0;
}

static int mali_c720_tpg_enum_mbus_code(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_mbus_code_enum *code)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;

	if (code->pad)
		return -EINVAL;

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_16BIT;

	cfg = mali_c720_get_mbus_config_by_matched_index(code->index, &match);
	if (!cfg)
		return -EINVAL;

	code->code = cfg->code;

	return 0;
}

static int mali_c720_tpg_enum_frame_size(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;

	if (fse->index || fse->pad)
		return -EINVAL;

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_16BIT;

	cfg = mali_c720_get_mbus_config_by_code(fse->code);
	if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
		return -EINVAL;

	fse->min_width = MALI_C720_MIN_INPUT_WIDTH;
	fse->max_width = MALI_C720_MAX_WIDTH;
	fse->min_height = MALI_C720_MIN_INPUT_HEIGHT;
	fse->max_height = MALI_C720_MAX_HEIGHT;

	return 0;
}

static int mali_c720_tpg_set_fmt(struct v4l2_subdev *sd,
				 const struct v4l2_subdev_client_info *ci,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *format)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_tpg *tpg = mali_c720_sd_to_sd(mdev, struct mali_c720_tpg);
	struct v4l2_mbus_framefmt *fmt;
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;
	unsigned int min_vblank, max_vblank, def_vblank;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	fmt = v4l2_subdev_state_get_format(state, MALI_C720_TPG_SRC_PAD);
	fmt->code = format->format.code;

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_16BIT;

	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
		cfg = mali_c720_get_mbus_config_by_code(MEDIA_BUS_FMT_SRGGB16_1X16);

	fmt->code = cfg->code;

	fmt->width = format->format.width;
	fmt->height = format->format.height;
	v4l_bound_align_image(&fmt->width, MALI_C720_MIN_INPUT_WIDTH,
			      MALI_C720_MAX_WIDTH, 2,
			      &fmt->height, MALI_C720_MIN_INPUT_HEIGHT,
			      MALI_C720_MAX_HEIGHT, 2, 0);

	format->format = *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	mali_c720_tpg_calculate_vblank(tpg, fmt->width, fmt->height,
				       &min_vblank, &max_vblank, &def_vblank);
	__v4l2_ctrl_modify_range(tpg->ctrls.vblank, min_vblank,
				 max_vblank, 1, def_vblank);

	return 0;
}

static int mali_c720_tpg_enable_streams(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					u32 pad, u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_tpg *tpg = mali_c720_sd_to_sd(mdev, struct mali_c720_tpg);
	int ret;

	ret = mali_c720_tpg_configure(tpg, state);
	if (ret)
		return ret;

	mali_c720_tpg_program_vertical_blank(tpg, state);
	mali_c720_tpg_program_test_pattern(tpg, (u32)tpg->ctrls.pattern->cur.val);
	mali_c720_tpg_start(tpg);

	return 0;
}

static int mali_c720_tpg_disable_streams(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 u32 pad, u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720 *mali_c720 = mdev->mali_c720;
	const struct mali_c720_pre_cm_regs *pre_cm = mali_c720->registers.top->pre_cm;
	struct mali_c720_tpg *tpg = mali_c720_sd_to_sd(mdev, struct mali_c720_tpg);

	mali_c720_tpg_stop(tpg);

	if (tpg->id == MALI_C720_TPG0) {
		mali_c720_write(mali_c720,
				&pre_cm->tpg_0_splitter_sel, 0x00);
	} else {
		mali_c720_write(mali_c720,
				&pre_cm->tpg_1_splitter_sel, 0x00);
	}

	mali_c720_write(mali_c720, &pre_cm->input_0_mux_sel, 0x00);

	tpg->remote = NULL;

	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c720_tpg_pad_ops = {
	.enum_mbus_code		= mali_c720_tpg_enum_mbus_code,
	.enum_frame_size	= mali_c720_tpg_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c720_tpg_set_fmt,
	.enable_streams		= mali_c720_tpg_enable_streams,
	.disable_streams	= mali_c720_tpg_disable_streams,
};

static const struct v4l2_subdev_ops mali_c720_tpg_ops = {
	.pad = &mali_c720_tpg_pad_ops,
};

static int mali_c720_tpg_init_state(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state,
					     MALI_C720_TPG_SRC_PAD,
					     MALI_C720_TPG_DEFAULT_STREAM);

	fmt->width = MALI_C720_DEFAULT_WIDTH;
	fmt->height = MALI_C720_DEFAULT_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = MEDIA_BUS_FMT_SRGGB16_1X16;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;

	return 0;
}

static const struct v4l2_subdev_internal_ops mali_c720_tpg_internal_ops = {
	.init_state = mali_c720_tpg_init_state,
};

static int mali_c720_tpg_init_controls(struct mali_c720_tpg *tpg)
{
	struct mali_c720 *mali_c720 = tpg->mdev.mali_c720;
	struct mali_c720_tpg_ctrls *ctrls = &tpg->ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	const s64 tpg_clk_rate = (s64)tpg->clk_rate;
	unsigned int min_vblank, max_vblank, def_vblank;
	int ret;

	ret = v4l2_ctrl_handler_init(&ctrls->handler, 4);
	if (ret)
		return ret;

	ctrls->pattern =
		v4l2_ctrl_new_std_menu_items(&ctrls->handler,
					     &mali_c720_tpg_ctrl_ops, V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(mali_c720_tpg_test_pattern_menu) - 1,
					     0, 3, mali_c720_tpg_test_pattern_menu);

	/*
	 * We fix hblank at the minimum allowed value and control framerate
	 * solely through the vblank control.
	 */
	hblank = v4l2_ctrl_new_std(&ctrls->handler, &mali_c720_tpg_ctrl_ops,
				   V4L2_CID_HBLANK, MALI_C720_TPG_FIXED_HBLANK,
				   MALI_C720_TPG_FIXED_HBLANK, 1,
				   MALI_C720_TPG_FIXED_HBLANK);
	if (hblank)
		hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	mali_c720_tpg_calculate_vblank(tpg, MALI_C720_DEFAULT_WIDTH, MALI_C720_DEFAULT_HEIGHT,
				       &min_vblank, &max_vblank, &def_vblank);

	ctrls->vblank = v4l2_ctrl_new_std(&ctrls->handler,
					  &mali_c720_tpg_ctrl_ops,
					  V4L2_CID_VBLANK,
					  min_vblank, max_vblank, 1, def_vblank);

	pixel_rate = v4l2_ctrl_new_std(&ctrls->handler, &mali_c720_tpg_ctrl_ops,
				       V4L2_CID_PIXEL_RATE,
				       tpg_clk_rate,
				       tpg_clk_rate, 1,
				       tpg_clk_rate);
	if (pixel_rate)
		pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrls->handler.error) {
		dev_err(mali_c720->dev, "Error during v4l2 controls init\n");
		v4l2_ctrl_handler_free(&ctrls->handler);
		return ctrls->handler.error;
	}

	tpg->mdev.sd.ctrl_handler = &ctrls->handler;
	tpg->mdev.sd.state_lock = ctrls->handler.lock;

	return 0;
}

static int mali_c720_tpg_subdev_link_setup(struct media_entity *entity,
					   const struct media_pad *local,
					   const struct media_pad *remote,
					   u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_tpg *tpg = mali_c720_sd_to_sd(mdev, struct mali_c720_tpg);
	struct media_pad *remote_sink;

	if (flags & MEDIA_LNK_FL_ENABLED) {
		remote_sink = media_pad_remote_pad_unique(local);

		/* No previous link connected */
		if (IS_ERR(remote_sink))
			return 0;

		if (remote_sink != remote) {
			dev_err(mdev->mali_c720->dev,
				"tpg %d source pad is already connected to another source\n",
				(tpg->id == MALI_C720_TPG0) ? 0 : 1);
			return -EBUSY;
		}
	}

	return 0;
}

static const struct media_entity_operations mali_c720_tpg_media_ops = {
	.link_setup		= mali_c720_tpg_subdev_link_setup,
	.link_validate		= v4l2_subdev_link_validate,
};

static int mali_c720_register_tpg(struct mali_c720 *mali_c720,
				  unsigned int index)
{
	struct mali_c720_tpg *tpg = &mali_c720->tpg[index];
	struct mali_c720_subdev *mdev = &tpg->mdev;
	struct v4l2_subdev *sd = &mdev->sd;
	int ret;

	mdev->mali_c720 = mali_c720;

	tpg->id = index;
	tpg->clk_rate = clk_get_rate(mali_c720->clk);
	v4l2_subdev_init(sd, &mali_c720_tpg_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &mali_c720_tpg_media_ops;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->internal_ops = &mali_c720_tpg_internal_ops;
	scnprintf(sd->name, sizeof(sd->name), "mali-c720 tpg %u", index);

	tpg->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity, 1, &tpg->pad);
	if (ret) {
		dev_err(mali_c720->dev,
			"Failed to initialize media entity pads\n");
		goto err_clear_parent;
	}

	ret = mali_c720_tpg_init_controls(tpg);
	if (ret) {
		dev_err(mali_c720->dev, "Error initialising controls\n");
		goto err_cleanup_media_entity;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_free_ctrl_handler;

	ret = v4l2_device_register_subdev(&mali_c720->v4l2_dev, sd);
	if (ret) {
		dev_err(mali_c720->dev, "Failed to register tpg subdev\n");
		goto err_cleanup_subdev;
	}

	return 0;

err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_free_ctrl_handler:
	v4l2_ctrl_handler_free(&tpg->ctrls.handler);
err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
err_clear_parent:
	mdev->mali_c720 = NULL;

	return ret;
}

static void mali_c720_unregister_tpg(struct mali_c720_tpg *tpg)
{
	struct mali_c720_subdev *mdev = &tpg->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_device_unregister_subdev(&mdev->sd);
}

static void mali_c720_cleanup_tpg(struct mali_c720_tpg *tpg)
{
	struct mali_c720_subdev *mdev = &tpg->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_ctrl_handler_free(&tpg->ctrls.handler);
	v4l2_subdev_cleanup(&mdev->sd);
	media_entity_cleanup(&mdev->sd.entity);
	mdev->mali_c720 = NULL;
}

int mali_c720_register_tpgs(struct mali_c720 *mali_c720)
{
	const struct mali_c720_discovery_target tpg = {
		.module_id = MALI_C720_DISC_MODULE_ID_TPG_RAW_MULTI,
		.pipeline_id = MALI_C720_DISC_PIPE_GLOBAL,
	};
	int ret;

	if (!mali_c720_discovery_module_present(mali_c720, &tpg))
		return 0;

	ret = mali_c720_register_tpg(mali_c720, MALI_C720_TPG0);
	if (ret)
		return ret;

	return mali_c720_register_tpg(mali_c720, MALI_C720_TPG1);
}

void mali_c720_unregister_tpgs(struct mali_c720 *mali_c720)
{
	for (int i = 0; i < MALI_C720_NUM_TPGS; i++)
		mali_c720_unregister_tpg(&mali_c720->tpg[i]);
}

void mali_c720_cleanup_tpgs(struct mali_c720 *mali_c720)
{
	for (int i = 0; i < MALI_C720_NUM_TPGS; i++)
		mali_c720_cleanup_tpg(&mali_c720->tpg[i]);
}
