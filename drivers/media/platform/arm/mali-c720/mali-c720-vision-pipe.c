// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>

#include "mali-c720-common.h"

static int mali_c720_vision_pipe_enum_mbus_code(struct v4l2_subdev *sd,
						struct v4l2_subdev_state *state,
						struct v4l2_subdev_mbus_code_enum *code)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;

	switch (code->pad) {
	case MALI_C720_VISION_PIPE_PAD_SOURCE_RGB:
		if (code->index)
			return -EINVAL;
		code->code = MEDIA_BUS_FMT_RGB121212_1X36;
		break;
	case MALI_C720_VISION_PIPE_PAD_SINK_VIDEO:
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_16BIT;

		cfg = mali_c720_get_mbus_config_by_matched_index(code->index, &match);
		if (!cfg)
			return -EINVAL;
		code->code = cfg->code;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mali_c720_vision_pipe_enum_frame_size(struct v4l2_subdev *sd,
						 struct v4l2_subdev_state *state,
						 struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;

	if (fse->index)
		return -EINVAL;

	switch (fse->pad) {
	case MALI_C720_VISION_PIPE_PAD_SOURCE_RGB:
		if (fse->code != MEDIA_BUS_FMT_RGB121212_1X36)
			return -EINVAL;
		break;
	case MALI_C720_VISION_PIPE_PAD_SINK_VIDEO:
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_16BIT;

		cfg = mali_c720_get_mbus_config_by_code(fse->code);
		if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	fse->min_width = MALI_C720_MIN_INPUT_WIDTH;
	fse->min_height = MALI_C720_MIN_INPUT_HEIGHT;
	fse->max_width = MALI_C720_MAX_WIDTH;
	fse->max_height = MALI_C720_MAX_HEIGHT;

	return 0;
}

static int mali_c720_vision_pipe_set_fmt(struct v4l2_subdev *sd,
					 const struct v4l2_subdev_client_info *ci,
					 struct v4l2_subdev_state *state,
					 struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *src_fmt, *sink_fmt;
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	/*
	 * Disallow set_fmt on the source pad; size is derived from the sink
	 * pad, while the RGB source format is fixed.
	 */
	if (format->pad != MALI_C720_VISION_PIPE_PAD_SINK_VIDEO)
		return v4l2_subdev_get_fmt(sd, state, format);

	sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_VISION_PIPE_PAD_SINK_VIDEO);

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_16BIT;

	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
		cfg = mali_c720_get_mbus_config_by_code(MEDIA_BUS_FMT_SRGGB16_1X16);

	sink_fmt->code = cfg->code;

	sink_fmt->width = fmt->width;
	sink_fmt->height = fmt->height;
	v4l_bound_align_image(&sink_fmt->width, MALI_C720_MIN_INPUT_WIDTH,
			      MALI_C720_MAX_WIDTH, 1,
			      &sink_fmt->height, MALI_C720_MIN_INPUT_HEIGHT,
			      MALI_C720_MAX_HEIGHT, 1, 0);

	*fmt = *sink_fmt;

	/* Propagate the input dimensions to the RGB output. */
	src_fmt = v4l2_subdev_state_get_format(state,
					       MALI_C720_VISION_PIPE_PAD_SOURCE_RGB);
	src_fmt->width = sink_fmt->width;
	src_fmt->height = sink_fmt->height;

	return 0;
}

static int mali_c720_vision_pipe_enable_streams(struct v4l2_subdev *sd,
						struct v4l2_subdev_state *state,
						u32 pad, u64 streams_mask)
{
	return 0;
}

static int mali_c720_vision_pipe_disable_streams(struct v4l2_subdev *sd,
						 struct v4l2_subdev_state *state,
						 u32 pad, u64 streams_mask)
{
	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c720_vision_pipe_pad_ops = {
	.enum_mbus_code		= mali_c720_vision_pipe_enum_mbus_code,
	.enum_frame_size	= mali_c720_vision_pipe_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c720_vision_pipe_set_fmt,
	.link_validate		= v4l2_subdev_link_validate_default,
	.enable_streams		= mali_c720_vision_pipe_enable_streams,
	.disable_streams	= mali_c720_vision_pipe_disable_streams,
};

static const struct v4l2_subdev_ops mali_c720_vision_pipe_ops = {
	.pad = &mali_c720_vision_pipe_pad_ops,
};

static int mali_c720_vision_pipe_init_state(struct v4l2_subdev *sd,
					    struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_VISION_PIPE_PAD_SINK_VIDEO);
	sink_fmt->width = MALI_C720_DEFAULT_WIDTH;
	sink_fmt->height = MALI_C720_DEFAULT_HEIGHT;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = MEDIA_BUS_FMT_SRGGB16_1X16;
	sink_fmt->colorspace = V4L2_COLORSPACE_RAW;
	sink_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(sink_fmt->colorspace);
	sink_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(sink_fmt->colorspace);
	sink_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;

	src_fmt = v4l2_subdev_state_get_format(state, MALI_C720_VISION_PIPE_PAD_SOURCE_RGB);
	*src_fmt = *sink_fmt;
	src_fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;
	src_fmt->colorspace = V4L2_COLORSPACE_SRGB;
	src_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(src_fmt->colorspace);
	src_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(src_fmt->colorspace);
	src_fmt->quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true, src_fmt->colorspace,
					      src_fmt->ycbcr_enc);

	return 0;
}

static const struct v4l2_subdev_internal_ops mali_c720_vision_pipe_internal_ops = {
	.init_state = mali_c720_vision_pipe_init_state,
};

static const struct media_entity_operations mali_c720_vision_pipe_media_ops = {
	.link_validate		= v4l2_subdev_link_validate,
};

int mali_c720_register_vision_pipe(struct mali_c720 *mali_c720)
{
	struct mali_c720_vision_pipe *pipe = &mali_c720->vision_pipe;
	struct mali_c720_subdev *mdev = &pipe->mdev;
	struct v4l2_subdev *sd = &mdev->sd;
	int ret;

	mdev->mali_c720 = mali_c720;

	v4l2_subdev_init(sd, &mali_c720_vision_pipe_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &mali_c720_vision_pipe_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->internal_ops = &mali_c720_vision_pipe_internal_ops;
	strscpy(sd->name, "mali-c720 hv pipe", sizeof(sd->name));

	pipe->pads[MALI_C720_VISION_PIPE_PAD_SINK_VIDEO].flags = MEDIA_PAD_FL_SINK |
								 MEDIA_PAD_FL_MUST_CONNECT;
	pipe->pads[MALI_C720_VISION_PIPE_PAD_SOURCE_RGB].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sd->entity,
				     MALI_C720_VISION_PIPE_NUM_PADS,
				     pipe->pads);
	if (ret)
		goto err_clear_parent;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_media_entity;

	ret = v4l2_device_register_subdev(&mali_c720->v4l2_dev, sd);
	if (ret)
		goto err_cleanup_subdev;

	return 0;

err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
err_clear_parent:
	mdev->mali_c720 = NULL;

	return ret;
}

void mali_c720_unregister_vision_pipe(struct mali_c720 *mali_c720)
{
	struct mali_c720_vision_pipe *pipe = &mali_c720->vision_pipe;
	struct mali_c720_subdev *mdev = &pipe->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_device_unregister_subdev(&mdev->sd);
}

void mali_c720_cleanup_vision_pipe(struct mali_c720 *mali_c720)
{
	struct mali_c720_vision_pipe *pipe = &mali_c720->vision_pipe;
	struct mali_c720_subdev *mdev = &pipe->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_subdev_cleanup(&mdev->sd);
	media_entity_cleanup(&mdev->sd.entity);
	mdev->mali_c720 = NULL;
}
