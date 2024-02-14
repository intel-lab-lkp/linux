// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Image signal processor
 *
 * Copyright (C) 2023 Ideas on Board Oy
 */

#include <linux/math.h>
#include <linux/minmax.h>

#include <media/media-entity.h>
#include <media/v4l2-subdev.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"
#include "mali-c55-resizer-coefs.h"

/* Scaling factor in Q4.20 format. */
#define MALI_C55_RZR_SCALER_FACTOR	1048576

static const u32 rzr_non_bypass_src_fmts[] = {
	MEDIA_BUS_FMT_RGB121212_1X36,
	MEDIA_BUS_FMT_YUV10_1X30
};

static const char * const mali_c55_resizer_names[] = {
	[MALI_C55_RZR_FR] = "resizer fr",
	[MALI_C55_RZR_DS] = "resizer ds",
};

/*
 * Inspect the routing table to know which of the two (mutually exclusive)
 * routes is enabled and return the sink pad id of the active route.
 */
static unsigned int mali_c55_rzr_get_active_sink(struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_krouting *routing = &state->routing;
	struct v4l2_subdev_route *route;

	/* A single route is enabled at a time. */
	for_each_active_route(routing, route)
		return route->sink_pad;

	return MALI_C55_RZR_SINK_PAD;
}

static int __mali_c55_rzr_set_routing(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_krouting *routing)
{
	struct mali_c55_resizer *rzr = container_of(sd, struct mali_c55_resizer,
						    sd);
	unsigned int active_sink = UINT_MAX;
	struct v4l2_rect *crop, *compose;
	struct v4l2_subdev_route *route;
	unsigned int active_routes = 0;
	struct v4l2_mbus_framefmt *fmt;
	int ret;

	ret = v4l2_subdev_routing_validate(sd, routing, 0);
	if (ret)
		return ret;

	/* Only a single route can be enabled at a time. */
	for_each_active_route(routing, route) {
		if (++active_routes > 1) {
			dev_err(rzr->mali_c55->dev,
				"Only one route can be active");
			return -EINVAL;
		}

		active_sink = route->sink_pad;
	}
	if (active_sink == UINT_MAX) {
		dev_err(rzr->mali_c55->dev, "One route has to be active");
		return -EINVAL;
	}

	ret = v4l2_subdev_set_routing(sd, state, routing);
	if (ret) {
		dev_err(rzr->mali_c55->dev, "Failed to set routing\n");
		return ret;
	}

	fmt = v4l2_subdev_state_get_format(state, active_sink, 0);
	crop = v4l2_subdev_state_get_crop(state, active_sink, 0);
	compose = v4l2_subdev_state_get_compose(state, active_sink, 0);

	fmt->width = MALI_C55_DEFAULT_WIDTH;
	fmt->height = MALI_C55_DEFAULT_HEIGHT;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->field = V4L2_FIELD_NONE;

	if (active_sink == MALI_C55_RZR_SINK_PAD) {
		fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;

		crop->left = crop->top = 0;
		crop->width = MALI_C55_DEFAULT_WIDTH;
		crop->height = MALI_C55_DEFAULT_HEIGHT;

		*compose = *crop;
	} else {
		fmt->code = MEDIA_BUS_FMT_SRGGB12_1X12;
	}

	/* Propagate the format to the source pad */
	*v4l2_subdev_state_get_format(state, MALI_C55_RZR_SOURCE_PAD, 0) = *fmt;

	return 0;
}

static int mali_c55_rzr_enum_mbus_code(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_mbus_code_enum *code)
{
	struct v4l2_mbus_framefmt *sink_fmt;
	const struct mali_c55_isp_fmt *fmt;
	unsigned int index = 0;
	u32 sink_pad;

	switch (code->pad) {
	case MALI_C55_RZR_SINK_PAD:
		if (code->index)
			return -EINVAL;

		code->code = MEDIA_BUS_FMT_RGB121212_1X36;

		return 0;
	case MALI_C55_RZR_SOURCE_PAD:
		sink_pad = mali_c55_rzr_get_active_sink(state);
		sink_fmt = v4l2_subdev_state_get_format(state, sink_pad, 0);

		/*
		 * If the active route is from the Bypass sink pad, then the
		 * source pad is a simple passthrough of the sink format.
		 */

		if (sink_pad == MALI_C55_RZR_SINK_BYPASS_PAD) {
			if (code->index)
				return -EINVAL;

			code->code = sink_fmt->code;
			return 0;
		}

		/*
		 * If the active route is from the non-bypass sink then we can
		 * select either RGB or conversion to YUV.
		 */

		if (code->index >= ARRAY_SIZE(rzr_non_bypass_src_fmts))
			return -EINVAL;

		code->code = rzr_non_bypass_src_fmts[code->index];

		return 0;
	case MALI_C55_RZR_SINK_BYPASS_PAD:
		for_each_mali_isp_fmt(fmt) {
			if (index++ == code->index) {
				code->code = fmt->code;
				return 0;
			}
		}

		break;
	}

	return -EINVAL;
}

static int mali_c55_rzr_enum_frame_size(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index)
		return -EINVAL;

	fse->max_width = MALI_C55_MAX_WIDTH;
	fse->max_height = MALI_C55_MAX_HEIGHT;
	fse->min_width = MALI_C55_MIN_WIDTH;
	fse->min_height = MALI_C55_MIN_HEIGHT;

	return 0;
}

static int mali_c55_rzr_set_sink_fmt(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_rect *rect;
	unsigned int sink_pad;

	/*
	 * Clamp to min/max and then reset crop and compose rectangles to the
	 * newly applied size.
	 */
	clamp_t(unsigned int, fmt->width,
		MALI_C55_MIN_WIDTH, MALI_C55_MAX_WIDTH);
	clamp_t(unsigned int, fmt->height,
		MALI_C55_MIN_HEIGHT, MALI_C55_MAX_HEIGHT);

	sink_pad = mali_c55_rzr_get_active_sink(state);
	if (sink_pad == MALI_C55_RZR_SINK_PAD) {
		fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;

		rect = v4l2_subdev_state_get_crop(state, MALI_C55_RZR_SINK_PAD);
		rect->left = 0;
		rect->top = 0;
		rect->width = fmt->width;
		rect->height = fmt->height;

		rect = v4l2_subdev_state_get_compose(state,
						     MALI_C55_RZR_SINK_PAD);
		rect->left = 0;
		rect->top = 0;
		rect->width = fmt->width;
		rect->height = fmt->height;
	} else {
		/*
		 * Make sure the media bus code is one of the supported
		 * ISP input media bus codes.
		 */
		if (!mali_c55_isp_is_format_supported(fmt->code))
			fmt->code = MALI_C55_DEFAULT_MEDIA_BUS_FMT;
	}

	*v4l2_subdev_state_get_format(state, sink_pad, 0) = *fmt;
	*v4l2_subdev_state_get_format(state, MALI_C55_RZR_SOURCE_PAD, 0) = *fmt;

	return 0;
}

static int mali_c55_rzr_set_source_fmt(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_format *format)
{
	struct mali_c55_resizer *rzr = container_of(sd, struct mali_c55_resizer,
						    sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_rect *crop, *compose;
	unsigned int sink_pad;
	unsigned int i;

	sink_pad = mali_c55_rzr_get_active_sink(state);
	sink_fmt = v4l2_subdev_state_get_format(state, sink_pad, 0);
	crop = v4l2_subdev_state_get_crop(state, sink_pad, 0);
	compose = v4l2_subdev_state_get_compose(state, sink_pad, 0);

	/* FR Bypass pipe. */

	if (sink_pad == MALI_C55_RZR_SINK_BYPASS_PAD) {
		/*
		 * Format on the source pad is the same as the one on the
		 * sink pad.
		 */
		fmt->code = sink_fmt->code;

		/* RAW bypass disables scaling and cropping. */
		crop->top = compose->top = 0;
		crop->left = compose->left = 0;
		fmt->width = crop->width = compose->width = sink_fmt->width;
		fmt->height = crop->height = compose->height = sink_fmt->height;

		*v4l2_subdev_state_get_format(state,
					      MALI_C55_RZR_SOURCE_PAD) = *fmt;

		return 0;
	}

	/* Regular processing pipe. */

	for (i = 0; i < ARRAY_SIZE(rzr_non_bypass_src_fmts); i++) {
		if (fmt->code == rzr_non_bypass_src_fmts[i])
			break;
	}

	if (i == ARRAY_SIZE(rzr_non_bypass_src_fmts)) {
		dev_dbg(rzr->mali_c55->dev,
			"Unsupported mbus code 0x%x: using default\n",
			fmt->code);
		fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;
	}

	/*
	 * The source pad format size comes directly from the sink pad
	 * compose rectangle.
	 */
	fmt->width = compose->width;
	fmt->height = compose->height;

	*v4l2_subdev_state_get_format(state, MALI_C55_RZR_SOURCE_PAD) = *fmt;

	return 0;
}

static int mali_c55_rzr_set_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *format)
{
	/*
	 * On sink pads fmt is either fixed for the 'regular' processing
	 * pad or a RAW format or 20-bit wide RGB/YUV format for the FR bypass
	 * pad.
	 *
	 * On source pad sizes are the result of crop+compose on the sink
	 * pad sizes, while the format depends on the active route.
	 */

	if (format->pad != MALI_C55_RZR_SOURCE_PAD)
		return mali_c55_rzr_set_sink_fmt(sd, state, format);

	return mali_c55_rzr_set_source_fmt(sd, state, format);
}

static int mali_c55_rzr_get_selection(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_selection *sel)
{
	if (sel->pad != MALI_C55_RZR_SINK_PAD)
		return -EINVAL;

	if (sel->target != V4L2_SEL_TGT_CROP &&
	    sel->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	sel->r = sel->target == V4L2_SEL_TGT_CROP
	       ? *v4l2_subdev_state_get_crop(state, MALI_C55_RZR_SINK_PAD)
	       : *v4l2_subdev_state_get_compose(state, MALI_C55_RZR_SINK_PAD);

	return 0;
}

static int mali_c55_rzr_set_selection(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *source_fmt;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_rect *crop, *compose;

	if (sel->pad != MALI_C55_RZR_SINK_PAD)
		return -EINVAL;

	if (sel->target != V4L2_SEL_TGT_CROP &&
	    sel->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	source_fmt = v4l2_subdev_state_get_format(state,
						  MALI_C55_RZR_SOURCE_PAD);
	sink_fmt = v4l2_subdev_state_get_format(state, MALI_C55_RZR_SINK_PAD);
	crop = v4l2_subdev_state_get_crop(state, MALI_C55_RZR_SINK_PAD);
	compose = v4l2_subdev_state_get_compose(state, MALI_C55_RZR_SINK_PAD);

	/* RAW bypass disables crop/scaling. */
	if (mali_c55_format_is_raw(source_fmt->code)) {
		crop->top = compose->top = 0;
		crop->left = compose->left = 0;
		crop->width = compose->width = sink_fmt->width;
		crop->height = compose->height = sink_fmt->height;

		sel->r = sel->target == V4L2_SEL_TGT_CROP ? *crop : *compose;

		return 0;
	}

	 /*
	  * Update the desired target and then clamp the crop rectangle to the
	  * sink format sizes and the compose size to the crop sizes.
	  */
	if (sel->target == V4L2_SEL_TGT_CROP)
		*crop = sel->r;
	else
		*compose = sel->r;

	clamp_t(unsigned int, crop->left, 0,  sink_fmt->width);
	clamp_t(unsigned int, crop->top, 0,  sink_fmt->height);
	clamp_t(unsigned int, crop->width, MALI_C55_MIN_WIDTH,
		sink_fmt->width - crop->left);
	clamp_t(unsigned int, crop->height, MALI_C55_MIN_HEIGHT,
		sink_fmt->height - crop->top);

	compose->left = 0;
	compose->top = 0;
	clamp_t(unsigned int, compose->left, 0,  sink_fmt->width);
	clamp_t(unsigned int, compose->top, 0,  sink_fmt->height);
	clamp_t(unsigned int, compose->width, crop->width / 8, crop->width);
	clamp_t(unsigned int, compose->height, crop->height / 8, crop->height);

	sel->r = sel->target == V4L2_SEL_TGT_CROP ? *crop : *compose;

	return 0;
}

static int mali_c55_rzr_set_routing(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    enum v4l2_subdev_format_whence which,
				    struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	return __mali_c55_rzr_set_routing(sd, state, routing);
}

static const struct v4l2_subdev_pad_ops mali_c55_resizer_pad_ops = {
	.enum_mbus_code		= mali_c55_rzr_enum_mbus_code,
	.enum_frame_size	= mali_c55_rzr_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c55_rzr_set_fmt,
	.get_selection		= mali_c55_rzr_get_selection,
	.set_selection		= mali_c55_rzr_set_selection,
	.set_routing		= mali_c55_rzr_set_routing,
};

void mali_c55_rzr_start_stream(struct mali_c55_resizer *rzr)
{
	struct mali_c55 *mali_c55 = rzr->mali_c55;
	struct v4l2_subdev *sd = &rzr->sd;
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop, *scale;
	unsigned int reg_offset;
	unsigned int sink_pad;
	u32 bypass = 0;

	reg_offset = rzr->cap_dev->reg_offset;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	sink_pad = mali_c55_rzr_get_active_sink(state);
	if (sink_pad == MALI_C55_RZR_SINK_BYPASS_PAD) {
		/* Bypass FR pipe processing if the bypass route is active. */
		mali_c55_update_bits(mali_c55, MALI_C55_REG_ISP_RAW_BYPASS,
				     MALI_C55_ISP_RAW_BYPASS_FR_BYPASS_MASK,
				     MALI_C55_ISP_RAW_BYPASS_RAW_FR_BYPASS);

		goto unlock_state;
	}

	/* Disable bypass and use regular processing. */
	mali_c55_update_bits(mali_c55, MALI_C55_REG_ISP_RAW_BYPASS,
			     MALI_C55_ISP_RAW_BYPASS_FR_BYPASS_MASK, 0);

	/* Verify if cropping and scaling should be enabled. */
	fmt = v4l2_subdev_state_get_format(state, MALI_C55_RZR_SINK_PAD, 0);
	crop = v4l2_subdev_state_get_crop(state, MALI_C55_RZR_SINK_PAD, 0);
	scale = v4l2_subdev_state_get_compose(state, MALI_C55_RZR_SINK_PAD, 0);

	if (fmt->width == crop->width && fmt->height == crop->height)
		bypass |= MALI_C55_BYPASS_CROP;
	if (crop->width == scale->width && crop->height == scale->height)
		bypass |= MALI_C55_BYPASS_SCALER;

	if (!(bypass & MALI_C55_BYPASS_CROP)) {
		mali_c55_write(mali_c55, MALI_C55_REG_CROP_EN(reg_offset),
			       MALI_C55_CROP_ENABLE);
		mali_c55_write(mali_c55, MALI_C55_REG_CROP_X_START(reg_offset),
			       crop->left);
		mali_c55_write(mali_c55, MALI_C55_REG_CROP_Y_START(reg_offset),
			       crop->top);
		mali_c55_write(mali_c55, MALI_C55_REG_CROP_X_SIZE(reg_offset),
			       crop->width);
		mali_c55_write(mali_c55, MALI_C55_REG_CROP_Y_SIZE(reg_offset),
			       crop->height);
	}

	if (!(bypass & MALI_C55_BYPASS_SCALER)) {
		/* Program the V/H scaling factor in Q4.20 format. */
		u64 h_scale = crop->width * MALI_C55_RZR_SCALER_FACTOR;
		u64 v_scale = crop->height * MALI_C55_RZR_SCALER_FACTOR;
		unsigned int h_bank, v_bank;

		do_div(h_scale, scale->width);
		do_div(v_scale, scale->height);

		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_IN_WIDTH(reg_offset),
			       crop->width);
		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_IN_HEIGHT(reg_offset),
			       crop->height);

		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_OUT_WIDTH(reg_offset),
			       scale->width);
		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_OUT_HEIGHT(reg_offset),
			       scale->height);

		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_HFILT_TINC(reg_offset),
			       h_scale);
		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_VFILT_TINC(reg_offset),
			       v_scale);

		h_bank = mali_c55_calculate_bank_num(mali_c55, crop->width,
						     scale->width);
		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_HFILT_COEF(reg_offset),
			       h_bank);

		v_bank = mali_c55_calculate_bank_num(mali_c55, crop->height,
						     scale->height);
		mali_c55_write(mali_c55,
			       MALI_C55_REG_SCALER_VFILT_COEF(reg_offset),
			       v_bank);
	}

	mali_c55_update_bits(mali_c55, rzr->id == MALI_C55_RZR_FR ?
			     MALI_C55_REG_FR_BYPASS : MALI_C55_REG_DS_BYPASS,
			     MALI_C55_BYPASS_CROP | MALI_C55_BYPASS_SCALER,
			     bypass);

unlock_state:
	v4l2_subdev_unlock_state(state);
}

static const struct v4l2_subdev_ops mali_c55_resizer_ops = {
	.pad	= &mali_c55_resizer_pad_ops,
};

static int mali_c55_rzr_init_state(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state)
{
	struct mali_c55_resizer *rzr = container_of(sd, struct mali_c55_resizer,
						    sd);
	struct v4l2_subdev_krouting routing = { };
	struct v4l2_subdev_route *routes;
	unsigned int i;
	int ret;

	routes = kcalloc(rzr->num_routes, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;

	for (i = 0; i < rzr->num_routes; ++i) {
		struct v4l2_subdev_route *route = &routes[i];

		route->sink_pad = i
				? MALI_C55_RZR_SINK_BYPASS_PAD
				: MALI_C55_RZR_SINK_PAD;
		route->source_pad = MALI_C55_RZR_SOURCE_PAD;
		if (route->sink_pad != MALI_C55_RZR_SINK_BYPASS_PAD)
			route->flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE;
	}

	routing.num_routes = rzr->num_routes;
	routing.routes = routes;

	ret = __mali_c55_rzr_set_routing(sd, state, &routing);
	kfree(routes);

	return ret;
}

static const struct v4l2_subdev_internal_ops mali_c55_resizer_internal_ops = {
	.init_state = mali_c55_rzr_init_state,
};

static void mali_c55_resizer_program_coefficients(struct mali_c55 *mali_c55,
						  unsigned int index)
{
	unsigned int scaler_filt_coefmem_addrs[][2] = {
		[MALI_C55_RZR_FR] = {
			0x034A8, /* hfilt */
			0x044A8  /* vfilt */
		},
		[MALI_C55_RZR_DS] = {
			0x014A8, /* hfilt */
			0x024A8  /* vfilt */
		},
	};
	unsigned int haddr = scaler_filt_coefmem_addrs[index][0];
	unsigned int vaddr = scaler_filt_coefmem_addrs[index][1];
	unsigned int i, j;

	for (i = 0; i < MALI_C55_RESIZER_COEFS_NUM_BANKS; i++) {
		for (j = 0; j < MALI_C55_RESIZER_COEFS_NUM_ENTRIES; j++) {
			mali_c55_write(mali_c55, haddr,
				mali_c55_scaler_h_filter_coefficients[i][j]);
			mali_c55_write(mali_c55, vaddr,
				mali_c55_scaler_v_filter_coefficients[i][j]);

			haddr += 4;
			vaddr += 4;
		}
	}
}

int mali_c55_register_resizers(struct mali_c55 *mali_c55)
{
	unsigned int i;
	int ret;

	for (i = 0; i < MALI_C55_NUM_RZRS; ++i) {
		struct mali_c55_resizer *rzr = &mali_c55->resizers[i];
		struct v4l2_subdev *sd = &rzr->sd;
		unsigned int num_pads = MALI_C55_RZR_NUM_PADS;

		rzr->id = i;

		if (rzr->id == MALI_C55_RZR_FR)
			rzr->cap_dev = &mali_c55->cap_devs[MALI_C55_CAP_DEV_FR];
		else
			rzr->cap_dev = &mali_c55->cap_devs[MALI_C55_CAP_DEV_DS];

		mali_c55_resizer_program_coefficients(mali_c55, i);

		v4l2_subdev_init(sd, &mali_c55_resizer_ops);
		sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE
			  | V4L2_SUBDEV_FL_STREAMS;
		sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_SCALER;
		sd->owner = THIS_MODULE;
		sd->internal_ops = &mali_c55_resizer_internal_ops;
		snprintf(sd->name, ARRAY_SIZE(sd->name), "%s %s",
			 MALI_C55_DRIVER_NAME, mali_c55_resizer_names[i]);

		rzr->pads[MALI_C55_RZR_SINK_PAD].flags = MEDIA_PAD_FL_SINK;
		rzr->pads[MALI_C55_RZR_SOURCE_PAD].flags = MEDIA_PAD_FL_SOURCE;

		/* Only the FR pipe has a bypass pad. */
		if (rzr->id == MALI_C55_RZR_FR) {
			rzr->pads[MALI_C55_RZR_SINK_BYPASS_PAD].flags =
							MEDIA_PAD_FL_SINK;
			rzr->num_routes = 2;
		} else {
			num_pads -= 1;
			rzr->num_routes = 1;
		}

		ret = media_entity_pads_init(&sd->entity, num_pads, rzr->pads);
		if (ret)
			return ret;

		ret = v4l2_subdev_init_finalize(sd);
		if (ret)
			goto err_cleanup;

		ret = v4l2_device_register_subdev(&mali_c55->v4l2_dev, sd);
		if (ret)
			goto err_cleanup;

		rzr->mali_c55 = mali_c55;
	}

	return 0;

err_cleanup:
	for (; i >= 0; --i) {
		struct mali_c55_resizer *rzr = &mali_c55->resizers[i];
		struct v4l2_subdev *sd = &rzr->sd;

		v4l2_subdev_cleanup(sd);
		media_entity_cleanup(&sd->entity);
	}

	return ret;
}

void mali_c55_unregister_resizers(struct mali_c55 *mali_c55)
{
	unsigned int i;

	for (i = 0; i < MALI_C55_NUM_RZRS; i++) {
		struct mali_c55_resizer *resizer = &mali_c55->resizers[i];

		if (!resizer->mali_c55)
			continue;

		v4l2_device_unregister_subdev(&resizer->sd);
		v4l2_subdev_cleanup(&resizer->sd);
		media_entity_cleanup(&resizer->sd.entity);
	}
}
