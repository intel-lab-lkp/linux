// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bitops.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>

#include "mali-c720-common.h"

static int mali_c720_isp_enum_mbus_code(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_mbus_code_enum *code)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct mali_c720_mbus_format_match match;

	/* HV uses the sink Bayer order at the internal 16-bit precision. */
	if (code->pad == MALI_C720_ISP_PAD_SOURCE_STATS) {
		if (code->index)
			return -EINVAL;
		code->code = MEDIA_BUS_FMT_METADATA_FIXED;
	} else if (code->pad == MALI_C720_ISP_PAD_SOURCE_HV) {
		if (code->index)
			return -EINVAL;

		sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SINK_VIDEO);
		if (!sink_fmt)
			return -EINVAL;

		cfg = mali_c720_get_mbus_config_by_code(sink_fmt->code);
		if (!cfg)
			return -EINVAL;

		cfg = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_16BIT);
		if (!cfg)
			return -EINVAL;

		code->code = cfg->code;
	} else if (code->pad == MALI_C720_ISP_PAD_SINK_VIDEO) {
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_24BIT;

		cfg = mali_c720_get_mbus_config_by_matched_index(code->index, &match);
		if (!cfg)
			return -EINVAL;

		code->code = cfg->code;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int mali_c720_isp_enum_frame_size(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct mali_c720_mbus_format_match match;

	if (fse->index)
		return -EINVAL;

	switch (fse->pad) {
	case MALI_C720_ISP_PAD_SOURCE_HV:
		sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SINK_VIDEO);
		if (!sink_fmt)
			return -EINVAL;

		cfg = mali_c720_get_mbus_config_by_code(sink_fmt->code);
		if (!cfg)
			return -EINVAL;

		cfg = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_16BIT);
		if (!cfg || fse->code != cfg->code)
			return -EINVAL;
		break;
	case MALI_C720_ISP_PAD_SINK_VIDEO:
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_24BIT;

		cfg = mali_c720_get_mbus_config_by_code(fse->code);
		if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
			return -EINVAL;
		break;
	case MALI_C720_ISP_PAD_SOURCE_STATS:
		return -ENOTTY;
	default:
		return -EINVAL;
	}

	fse->min_width = MALI_C720_MIN_INPUT_WIDTH;
	fse->min_height = MALI_C720_MIN_INPUT_HEIGHT;
	fse->max_width = MALI_C720_MAX_WIDTH;
	fse->max_height = MALI_C720_MAX_HEIGHT;

	return 0;
}

static int mali_c720_isp_set_fmt(struct v4l2_subdev *sd,
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

	/* Only the video sink pad format can be changed. */
	if (format->pad != MALI_C720_ISP_PAD_SINK_VIDEO)
		return v4l2_subdev_get_fmt(sd, state, format);

	sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SINK_VIDEO);
	if (!sink_fmt)
		return -EINVAL;

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_24BIT;

	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
		cfg = mali_c720_get_mbus_config_by_code(MEDIA_BUS_FMT_SRGGB24_1X24);

	sink_fmt->code = cfg->code;

	sink_fmt->width = fmt->width;
	sink_fmt->height = fmt->height;
	v4l_bound_align_image(&sink_fmt->width, MALI_C720_MIN_INPUT_WIDTH,
			      MALI_C720_MAX_WIDTH, 2,
			      &sink_fmt->height, MALI_C720_MIN_INPUT_HEIGHT,
			      MALI_C720_MAX_HEIGHT, 2, 0);

	*fmt = *sink_fmt;

	/* Propagate the input size and Bayer order to the HV output. */
	src_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SOURCE_HV);
	src_fmt->width = sink_fmt->width;
	src_fmt->height = sink_fmt->height;
	src_fmt->code = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_16BIT)->code;

	return 0;
}

static void mali_c720_isp_stop(struct mali_c720 *mali_c720)
{
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);

	mali_c720_ctx_mgr_stop_context(ctx);
}

static int mali_c720_isp_start(struct mali_c720_isp *isp,
			       struct v4l2_subdev_state *state)
{
	struct mali_c720 *mali_c720 = isp->mdev.mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	const struct mali_c720_mbus_format_info *cfg;
	const struct v4l2_mbus_framefmt *fmt;
	struct mali_c720_top_cfg top_cfg;
	struct media_pad *sink_pad;
	struct v4l2_subdev *src_sd;
	int ret;

	fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SINK_VIDEO);
	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg)
		return -EINVAL;

	top_cfg.width = fmt->width;
	top_cfg.height = fmt->height;
	top_cfg.rggb_start = (u32)__ffs(cfg->type);

	mali_c720_ctx_mgr_configure_context(ctx, &top_cfg);

	sink_pad = &isp->pads[MALI_C720_ISP_PAD_SINK_VIDEO];
	isp->remote_src = media_pad_remote_pad_unique(sink_pad);
	ret = PTR_ERR_OR_ZERO(isp->remote_src);
	if (ret)
		return ret;

	src_sd = media_entity_to_v4l2_subdev(isp->remote_src->entity);
	ret = v4l2_subdev_enable_streams(src_sd, isp->remote_src->index, BIT(0));
	if (ret && ret != -EALREADY) {
		dev_err(mali_c720->dev, "Failed to start ISP source\n");
		return ret;
	}

	return 0;
}

static int mali_c720_isp_enable_streams(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state, u32 pad,
					u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_isp *isp = mali_c720_sd_to_sd(mdev, struct mali_c720_isp);
	int ret;

	/* Enable the stream but only start the ISP once. */
	if (isp->enabled)
		return 0;

	ret = mali_c720_isp_start(isp, state);
	if (ret)
		return ret;

	isp->enabled = true;

	return 0;
}

static int mali_c720_isp_disable_streams(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state, u32 pad,
					 u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_isp *isp = mali_c720_sd_to_sd(mdev, struct mali_c720_isp);
	struct mali_c720 *mali_c720 = mdev->mali_c720;
	struct v4l2_subdev *src_sd;

	if (!isp->enabled)
		return 0;

	mali_c720_isp_stop(mali_c720);

	if (isp->remote_src) {
		src_sd = media_entity_to_v4l2_subdev(isp->remote_src->entity);
		v4l2_subdev_disable_streams(src_sd, isp->remote_src->index,
					    BIT(0));
	}
	isp->remote_src = NULL;
	isp->enabled = false;

	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c720_isp_pad_ops = {
	.enum_mbus_code		= mali_c720_isp_enum_mbus_code,
	.enum_frame_size	= mali_c720_isp_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c720_isp_set_fmt,
	.link_validate		= v4l2_subdev_link_validate_default,
	.enable_streams		= mali_c720_isp_enable_streams,
	.disable_streams	= mali_c720_isp_disable_streams,
};

void mali_c720_isp_queue_event_sof(struct mali_c720 *mali_c720, u32 sequence)
{
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct mali_c720_subdev *mdev = &isp->mdev;

	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};

	event.u.frame_sync.frame_sequence = sequence;
	v4l2_event_queue(mdev->sd.devnode, &event);
}

static int
mali_c720_isp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			      struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static const struct v4l2_subdev_core_ops mali_c720_isp_core_ops = {
	.subscribe_event = mali_c720_isp_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops mali_c720_isp_ops = {
	.pad	= &mali_c720_isp_pad_ops,
	.core	= &mali_c720_isp_core_ops,
};

static int mali_c720_isp_init_state(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt, *stats_fmt;

	sink_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SINK_VIDEO);
	sink_fmt->width = MALI_C720_DEFAULT_WIDTH;
	sink_fmt->height = MALI_C720_DEFAULT_HEIGHT;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = MEDIA_BUS_FMT_SRGGB24_1X24;
	sink_fmt->colorspace = V4L2_COLORSPACE_RAW;
	sink_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(sink_fmt->colorspace);
	sink_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(sink_fmt->colorspace);
	sink_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;

	src_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SOURCE_HV);
	*src_fmt = *sink_fmt;
	src_fmt->code = MEDIA_BUS_FMT_SRGGB16_1X16;

	stats_fmt = v4l2_subdev_state_get_format(state, MALI_C720_ISP_PAD_SOURCE_STATS);
	stats_fmt->width = 0;
	stats_fmt->height = 0;
	stats_fmt->field = V4L2_FIELD_NONE;
	stats_fmt->code = MEDIA_BUS_FMT_METADATA_FIXED;

	return 0;
}

static const struct v4l2_subdev_internal_ops mali_c720_isp_internal_ops = {
	.init_state = mali_c720_isp_init_state,
};

static int mali_c720_isp_subdev_link_setup(struct media_entity *entity,
					   const struct media_pad *local,
					   const struct media_pad *remote,
					   u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct mali_c720 *mali_c720 = v4l2_sd_to_mali_c720_sd(sd)->mali_c720;
	struct media_pad *remote_src;

	if (local->index != MALI_C720_ISP_PAD_SINK_VIDEO)
		return 0;

	if (flags & MEDIA_LNK_FL_ENABLED) {
		remote_src = media_pad_remote_pad_unique(local);

		/* No previous link connected */
		if (IS_ERR(remote_src))
			return 0;

		if (remote_src != remote) {
			dev_err(mali_c720->dev,
				"ISP sink pad %u is already connected to another source\n",
				local->index);
			return -EBUSY;
		}
	}

	return 0;
}

static const struct media_entity_operations mali_c720_isp_media_ops = {
	.link_setup		= mali_c720_isp_subdev_link_setup,
	.link_validate		= v4l2_subdev_link_validate,
};

int mali_c720_register_isp(struct mali_c720 *mali_c720)
{
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct mali_c720_subdev *mdev = &isp->mdev;
	struct v4l2_subdev *sd = &mdev->sd;
	int ret;

	mdev->mali_c720 = mali_c720;

	v4l2_subdev_init(sd, &mali_c720_isp_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->entity.ops = &mali_c720_isp_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->internal_ops = &mali_c720_isp_internal_ops;
	strscpy(sd->name, "mali-c720 isp", sizeof(sd->name));

	isp->pads[MALI_C720_ISP_PAD_SINK_VIDEO].flags = MEDIA_PAD_FL_SINK |
							MEDIA_PAD_FL_MUST_CONNECT;
	isp->pads[MALI_C720_ISP_PAD_SOURCE_HV].flags = MEDIA_PAD_FL_SOURCE;
	isp->pads[MALI_C720_ISP_PAD_SOURCE_STATS].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity,
				     MALI_C720_ISP_NUM_PADS,
				     isp->pads);
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

void mali_c720_unregister_isp(struct mali_c720 *mali_c720)
{
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct mali_c720_subdev *mdev = &isp->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_device_unregister_subdev(&mdev->sd);
}

void mali_c720_cleanup_isp(struct mali_c720 *mali_c720)
{
	struct mali_c720_isp *isp = &mali_c720->isp;
	struct mali_c720_subdev *mdev = &isp->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_subdev_cleanup(&mdev->sd);
	media_entity_cleanup(&mdev->sd.entity);
	mdev->mali_c720 = NULL;
}
