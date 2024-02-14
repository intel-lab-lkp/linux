// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Test pattern generator
 *
 * Copyright (C) 2023 Ideas on Board Oy
 */

#include <linux/minmax.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"

#define MALI_C55_TPG_SRC_PAD		0
#define MALI_C55_TPG_FIXED_HBLANK	0x20
#define MALI_C55_TPG_MAX_VBLANK		0xFFFF
#define MALI_C55_TPG_PIXEL_RATE		100000000

static const char * const mali_c55_tpg_test_pattern_menu[] = {
	"Flat field",
	"Horizontal gradient",
	"Vertical gradient",
	"Vertical bars",
	"Arbitrary rectangle",
	"White frame on black field"
};

static const u32 mali_c55_tpg_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB16_1X16,
	/*
	 * This is a lie. In RGB mode the Test Pattern Generator actually output
	 * 16-bits-per-colour data. However, RGB data follows one of the Bypass
	 * paths which has a 12-bit limit at the insertion point, meaning it
	 * would be truncated there to match the internal 12-bit format that
	 * would be output from the debayering block. The same is true of RGB
	 * data output by a sensor and streamed to the ISP's input port, however
	 * in that case the ISP's input port requires that data be converted to
	 * a 20-bit MSB aligned format. Given:
	 *
	 *	1. Our chosen topology represents the TPG as a subdevice
	 *	   linked to the ISP's input port.
	 *	2. We need to restrict the ISP's sink pad to only accepting that
	 *	   20-bit RGB format from sensors / CSI-2 receivers.
	 *	3. All the data ultimately ends up in the same format anyway and
	 *	   these data from the TPG are purely internal to the ISP
	 *
	 * It seems best to reduce the programming complexity by simply
	 * pretending that the TPG outputs data in the same format that the ISP
	 * input port requires, even though it doesn't really.
	 */
	MEDIA_BUS_FMT_RGB202020_1X60,
};

static void __mali_c55_tpg_calc_vblank(struct v4l2_mbus_framefmt *format,
				       int *def_vblank, int *min_vblank)
{
	unsigned int hts;
	int tgt_fps;
	int vblank;

	hts = format->width + MALI_C55_TPG_FIXED_HBLANK;

	/*
	 * The ISP has minimum vertical blanking requirements that must be
	 * adhered to by the TPG. The minimum is a function of the Iridix blocks
	 * clocking requirements and the width of the image and horizontal
	 * blanking, but if we assume the worst case iVariance and sVariance
	 * values then it boils down to the below.
	 */
	*min_vblank = 15 + (120500 / hts);

	/*
	 * We need to set a sensible default vblank for whatever format height
	 * we happen to be given from set_fmt(). This function just targets
	 * an even multiple of 15fps. If we can't get 15fps, let's target 5fps.
	 * If we can't get 5fps we'll take whatever the minimum vblank gives us.
	 */
	tgt_fps = MALI_C55_TPG_PIXEL_RATE / hts / (format->height + *min_vblank);

	if (tgt_fps < 5)
		vblank = *min_vblank;
	else
		vblank = MALI_C55_TPG_PIXEL_RATE / hts
		       / max(rounddown(tgt_fps, 15), 5);

	*def_vblank = ALIGN_DOWN(vblank, 2) - format->height;
}

static int mali_c55_tpg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mali_c55_tpg *tpg = container_of(ctrl->handler,
						struct mali_c55_tpg,
						ctrls.handler);
	struct mali_c55 *mali_c55 = container_of(tpg, struct mali_c55, tpg);

	switch (ctrl->id) {
	case V4L2_CID_TEST_PATTERN:
		mali_c55_write(mali_c55, MALI_C55_REG_TEST_GEN_CH0_PATTERN_TYPE,
			       ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		mali_c55_update_bits(mali_c55, MALI_C55_REG_BLANKING,
				     MALI_C55_REG_VBLANK_MASK, ctrl->val << 16);
		break;
	default:
		dev_err(mali_c55->dev, "invalid V4L2 control ID\n");
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops mali_c55_tpg_ctrl_ops = {
	.s_ctrl = &mali_c55_tpg_s_ctrl,
};

static void mali_c55_tpg_configure(struct mali_c55 *mali_c55,
				   struct v4l2_subdev *sd)
{
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;

	/*
	 * hblank needs setting, but is a read-only control and thus won't be
	 * called during __v4l2_ctrl_handler_setup(). Do it here instead.
	 */
	mali_c55_update_bits(mali_c55, MALI_C55_REG_BLANKING,
			     MALI_C55_REG_HBLANK_MASK,
			     MALI_C55_TPG_FIXED_HBLANK);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_GEN_VIDEO,
			     MALI_C55_REG_GEN_VIDEO_MULTI_MASK,
			     MALI_C55_REG_GEN_VIDEO_MULTI_MASK);

	state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_state_get_format(state, MALI_C55_TPG_SRC_PAD);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_TPG_CH0,
			     MALI_C5_TEST_PATTERN_RGB,
			     fmt->code == MEDIA_BUS_FMT_RGB202020_1X60 ?
					  MALI_C5_TEST_PATTERN_RGB : 0x0);

	v4l2_subdev_unlock_state(state);
}

static int mali_c55_tpg_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mali_c55_tpg *tpg = container_of(sd, struct mali_c55_tpg, sd);
	struct mali_c55 *mali_c55 = container_of(tpg, struct mali_c55, tpg);

	if (!enable) {
		mali_c55_update_bits(mali_c55, MALI_C55_REG_TPG_CH0,
				     MALI_C55_TEST_PATTERN_ON_OFF, 0x00);
		mali_c55_update_bits(mali_c55, MALI_C55_REG_GEN_VIDEO,
				     MALI_C55_REG_GEN_VIDEO_ON_MASK, 0x00);
	} else {
		/*
		 * One might reasonably expect the framesize to be set here
		 * given it's configurable in .set_fmt(), but it's done in the
		 * ISP subdevice's .s_stream() instead, as the same register is
		 * also used to indicate the size of the data coming from the
		 * sensor.
		 */
		mali_c55_tpg_configure(mali_c55, sd);
		__v4l2_ctrl_handler_setup(sd->ctrl_handler);

		mali_c55_update_bits(mali_c55, MALI_C55_REG_TPG_CH0,
				     MALI_C55_TEST_PATTERN_ON_OFF,
				     MALI_C55_TEST_PATTERN_ON_OFF);
		mali_c55_update_bits(mali_c55, MALI_C55_REG_GEN_VIDEO,
				     MALI_C55_REG_GEN_VIDEO_ON_MASK,
				     MALI_C55_REG_GEN_VIDEO_ON_MASK);
	}

	return 0;
}

static const struct v4l2_subdev_video_ops mali_c55_tpg_video_ops = {
	.s_stream = &mali_c55_tpg_s_stream,
};

static int mali_c55_tpg_enum_mbus_code(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad >= sd->entity.num_pads)
		return -EINVAL;

	if (code->index >= ARRAY_SIZE(mali_c55_tpg_mbus_codes))
		return -EINVAL;

	code->code = mali_c55_tpg_mbus_codes[code->index];

	return 0;
}

static int mali_c55_tpg_enum_frame_size(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0 || fse->pad > sd->entity.num_pads)
		return -EINVAL;

	fse->min_width = MALI_C55_MIN_WIDTH;
	fse->max_width = MALI_C55_MAX_WIDTH;
	fse->min_height = MALI_C55_MIN_HEIGHT;
	fse->max_height = MALI_C55_MAX_HEIGHT;

	return 0;
}

static int mali_c55_tpg_set_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *format)
{
	struct mali_c55_tpg *tpg = container_of(sd, struct mali_c55_tpg, sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	int vblank_def, vblank_min;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c55_tpg_mbus_codes); i++) {
		if (fmt->code == mali_c55_tpg_mbus_codes[i])
			break;
	}

	if (i == ARRAY_SIZE(mali_c55_tpg_mbus_codes))
		fmt->code = MEDIA_BUS_FMT_SRGGB16_1X16;

	/*
	 * The TPG says that the test frame timing generation logic expects a
	 * minimum framesize of 4x4 pixels, but given the rest of the ISP can't
	 * handle anything smaller than 128x128 it seems pointless to allow a
	 * smaller frame.
	 */
	clamp_t(unsigned int, fmt->width, MALI_C55_MIN_WIDTH,
		MALI_C55_MAX_WIDTH);
	clamp_t(unsigned int, fmt->height, MALI_C55_MIN_HEIGHT,
		MALI_C55_MAX_HEIGHT);

	*v4l2_subdev_state_get_format(state, MALI_C55_TPG_SRC_PAD) = *fmt;

	__mali_c55_tpg_calc_vblank(fmt, &vblank_def, &vblank_min);
	__v4l2_ctrl_modify_range(tpg->ctrls.vblank, vblank_min,
				 MALI_C55_TPG_MAX_VBLANK, 1, vblank_def);
	__v4l2_ctrl_s_ctrl(tpg->ctrls.vblank, vblank_def);

	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c55_tpg_pad_ops = {
	.enum_mbus_code		= mali_c55_tpg_enum_mbus_code,
	.enum_frame_size	= mali_c55_tpg_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c55_tpg_set_fmt,
};

static const struct v4l2_subdev_ops mali_c55_tpg_ops = {
	.video	= &mali_c55_tpg_video_ops,
	.pad	= &mali_c55_tpg_pad_ops,
};

static int mali_c55_tpg_init_state(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(sd_state, MALI_C55_TPG_SRC_PAD);

	fmt->width = MALI_C55_DEFAULT_WIDTH;
	fmt->height = MALI_C55_DEFAULT_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->code = MEDIA_BUS_FMT_SRGGB16_1X16;

	return 0;
}

static const struct v4l2_subdev_internal_ops mali_c55_tpg_internal_ops = {
	.init_state = mali_c55_tpg_init_state,
};

static int mali_c55_tpg_init_controls(struct mali_c55 *mali_c55)
{
	struct mali_c55_tpg_ctrls *ctrls = &mali_c55->tpg.ctrls;
	struct v4l2_subdev *sd = &mali_c55->tpg.sd;
	struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	int vblank_def, vblank_min;
	int ret;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	format = v4l2_subdev_state_get_format(state, MALI_C55_TPG_SRC_PAD);

	ret = v4l2_ctrl_handler_init(&ctrls->handler, 1);
	if (ret)
		goto err_unlock;

	ctrls->test_pattern = v4l2_ctrl_new_std_menu_items(&ctrls->handler,
				&mali_c55_tpg_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(mali_c55_tpg_test_pattern_menu) - 1,
				0, 3, mali_c55_tpg_test_pattern_menu);

	/*
	 * We fix hblank at the minimum allowed value and control framerate
	 * solely through the vblank control.
	 */
	ctrls->hblank = v4l2_ctrl_new_std(&ctrls->handler,
				&mali_c55_tpg_ctrl_ops,
				V4L2_CID_HBLANK, MALI_C55_TPG_FIXED_HBLANK,
				MALI_C55_TPG_FIXED_HBLANK, 1,
				MALI_C55_TPG_FIXED_HBLANK);
	if (ctrls->hblank)
		ctrls->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	__mali_c55_tpg_calc_vblank(format, &vblank_def, &vblank_min);
	ctrls->vblank = v4l2_ctrl_new_std(&ctrls->handler,
					  &mali_c55_tpg_ctrl_ops,
					  V4L2_CID_VBLANK, vblank_min,
					  MALI_C55_TPG_MAX_VBLANK, 1,
					  vblank_def);

	if (ctrls->handler.error) {
		dev_err(mali_c55->dev, "Error during v4l2 controls init\n");
		ret = ctrls->handler.error;
		goto err_free_handler;
	}

	ctrls->handler.lock = &mali_c55->tpg.lock;
	mali_c55->tpg.sd.ctrl_handler = &ctrls->handler;

	v4l2_subdev_unlock_state(state);

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctrls->handler);
err_unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

int mali_c55_register_tpg(struct mali_c55 *mali_c55)
{
	struct mali_c55_tpg *tpg = &mali_c55->tpg;
	struct v4l2_subdev *sd = &tpg->sd;
	struct media_pad *pad = &tpg->pad;
	int ret;

	mutex_init(&tpg->lock);

	v4l2_subdev_init(sd, &mali_c55_tpg_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->owner = THIS_MODULE;
	sd->internal_ops = &mali_c55_tpg_internal_ops;
	strscpy(sd->name, MALI_C55_DRIVER_NAME " tpg", sizeof(sd->name));

	pad->flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;
	ret = media_entity_pads_init(&sd->entity, 1, pad);
	if (ret) {
		dev_err(mali_c55->dev,
			"Failed to initialize media entity pads\n");
		goto err_destroy_mutex;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_media_entity;

	ret = mali_c55_tpg_init_controls(mali_c55);
	if (ret) {
		dev_err(mali_c55->dev,
			"Error initialising controls\n");
		goto err_cleanup_subdev;
	}

	ret = v4l2_device_register_subdev(&mali_c55->v4l2_dev, sd);
	if (ret) {
		dev_err(mali_c55->dev, "Failed to register tpg subdev\n");
		goto err_free_ctrl_handler;
	}

	/*
	 * By default the colour settings lead to a very dim image that is
	 * nearly indistinguishable from black on some monitor settings. Ramp
	 * them up a bit so the image is brighter.
	 */
	mali_c55_write(mali_c55, MALI_C55_REG_TPG_R_BACKGROUND,
		       MALI_C55_TPG_BACKGROUND_MAX);
	mali_c55_write(mali_c55, MALI_C55_REG_TPG_G_BACKGROUND,
		       MALI_C55_TPG_BACKGROUND_MAX);
	mali_c55_write(mali_c55, MALI_C55_REG_TPG_B_BACKGROUND,
		       MALI_C55_TPG_BACKGROUND_MAX);

	tpg->mali_c55 = mali_c55;

	return 0;

err_free_ctrl_handler:
	v4l2_ctrl_handler_free(&tpg->ctrls.handler);
err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
err_destroy_mutex:
	mutex_destroy(&tpg->lock);

	return ret;
}

void mali_c55_unregister_tpg(struct mali_c55 *mali_c55)
{
	struct mali_c55_tpg *tpg = &mali_c55->tpg;

	if (!tpg->mali_c55)
		return;

	v4l2_device_unregister_subdev(&tpg->sd);
	v4l2_subdev_cleanup(&tpg->sd);
	media_entity_cleanup(&tpg->sd.entity);
	v4l2_ctrl_handler_free(&tpg->ctrls.handler);
	mutex_destroy(&tpg->lock);
}
