// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>

#include "mali-c720-common.h"

/*
 * Return the active route for the provided sink pad and stream, or NULL if
 * there is none.
 */
static struct v4l2_subdev_route *
mali_c720_input_get_active_route(struct v4l2_subdev_state *state,
				 u32 sink_pad, u32 sink_stream)
{
	struct v4l2_subdev_krouting *routing = &state->routing;
	struct v4l2_subdev_route *route;

	for_each_active_route(routing, route) {
		if (route->sink_pad == sink_pad &&
		    route->sink_stream == sink_stream)
			return route;
	}

	return NULL;
}

static int mali_c720_input_enum_mbus_code(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state,
					  struct v4l2_subdev_mbus_code_enum *code)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_subdev_route *route;
	bool found = false;

	/*
	 * Sink pads support all bayer formats.
	 * The source format is propagated from the sink pad, shifted to 24-bit.
	 */
	if (code->pad != MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT) {
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_8BIT | MALI_C720_FORMAT_10BIT |
				   MALI_C720_FORMAT_12BIT | MALI_C720_FORMAT_14BIT |
				   MALI_C720_FORMAT_16BIT | MALI_C720_FORMAT_20BIT |
				   MALI_C720_FORMAT_24BIT;

		cfg = mali_c720_get_mbus_config_by_matched_index(code->index, &match);
		if (!cfg)
			return -EINVAL;

		code->code = cfg->code;

		return 0;
	}

	if (code->index)
		return -EINVAL;

	/*
	 * All routes from a source pad use the same format, so the first active
	 * route represents the source format.
	 */
	for_each_active_route(&state->routing, route) {
		if (route->source_pad == code->pad) {
			found = true;
			break;
		}
	}

	if (!found)
		return -EINVAL;

	sink_fmt = v4l2_subdev_state_get_format(state,
						route->sink_pad,
						route->sink_stream);
	if (!sink_fmt)
		return -EINVAL;

	cfg = mali_c720_get_mbus_config_by_code(sink_fmt->code);
	if (!cfg)
		return -EINVAL;

	cfg = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_24BIT);
	if (!cfg)
		return -EINVAL;

	code->code = cfg->code;

	return 0;
}

static int mali_c720_input_enum_frame_size(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state,
					   struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_subdev_route *route;
	bool found = false;

	if (fse->index)
		return -EINVAL;

	if (fse->pad != MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT) {
		match.type_mask = MALI_C720_FORMAT_BAYER;
		match.depth_mask = MALI_C720_FORMAT_8BIT | MALI_C720_FORMAT_10BIT |
				   MALI_C720_FORMAT_12BIT | MALI_C720_FORMAT_14BIT |
				   MALI_C720_FORMAT_16BIT | MALI_C720_FORMAT_20BIT |
				   MALI_C720_FORMAT_24BIT;

		cfg = mali_c720_get_mbus_config_by_code(fse->code);
		if (!cfg || !mali_c720_validate_mbus_format_match(cfg, &match))
			return -EINVAL;

		fse->min_width = MALI_C720_MIN_INPUT_WIDTH;
		fse->min_height = MALI_C720_MIN_INPUT_HEIGHT;
		fse->max_width = MALI_C720_MAX_WIDTH;
		fse->max_height = MALI_C720_MAX_HEIGHT;

		return 0;
	}

	for_each_active_route(&state->routing, route) {
		if (route->source_pad == fse->pad) {
			found = true;
			break;
		}
	}

	if (!found)
		return -EINVAL;

	sink_fmt = v4l2_subdev_state_get_format(state,
						route->sink_pad,
						route->sink_stream);
	if (!sink_fmt)
		return -EINVAL;

	cfg = mali_c720_get_mbus_config_by_code(sink_fmt->code);
	if (!cfg)
		return -EINVAL;

	cfg = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_24BIT);
	if (!cfg || fse->code != cfg->code)
		return -EINVAL;

	fse->min_width = MALI_C720_MIN_INPUT_WIDTH;
	fse->min_height = MALI_C720_MIN_INPUT_HEIGHT;
	fse->max_width = MALI_C720_MAX_WIDTH;
	fse->max_height = MALI_C720_MAX_HEIGHT;

	return 0;
}

static int mali_c720_input_set_fmt(struct v4l2_subdev *sd,
				   const struct v4l2_subdev_client_info *ci,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *src_fmt, *sink_fmt;
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_mbus_format_match match;
	struct v4l2_subdev_route *active_route;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	/*
	 * Disallow set_fmt on the source pad; format is derived from the
	 * sink pad format.
	 */
	if (format->pad == MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT)
		return v4l2_subdev_get_fmt(sd, state, format);

	sink_fmt = v4l2_subdev_state_get_format(state,
						format->pad,
						format->stream);
	if (!sink_fmt)
		return -EINVAL;

	match.type_mask = MALI_C720_FORMAT_BAYER;
	match.depth_mask = MALI_C720_FORMAT_8BIT | MALI_C720_FORMAT_10BIT |
			   MALI_C720_FORMAT_12BIT | MALI_C720_FORMAT_14BIT |
			   MALI_C720_FORMAT_16BIT | MALI_C720_FORMAT_20BIT |
			   MALI_C720_FORMAT_24BIT;

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

	/* The sink format should be propagated to the source pad
	 * if there is an active route between them.
	 * The data is shifted to 24-bits.
	 */

	active_route = mali_c720_input_get_active_route(state,
							format->pad,
							format->stream);
	if (!active_route)
		return 0;

	src_fmt = v4l2_subdev_state_get_format(state, active_route->source_pad);
	src_fmt->code = mali_c720_shift_mbus_format(cfg, MALI_C720_FORMAT_24BIT)->code;
	src_fmt->width = sink_fmt->width;
	src_fmt->height = sink_fmt->height;

	return 0;
}

static void mali_c720_free_input_ports(struct mali_c720_input *input)
{
	struct mali_c720 *mali_c720 = input->mdev.mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_slot_input *slot_input = &input->slot_input;
	struct mali_c720_buffer_pool *pool = &slot_input->raw_buffer_desc;
	unsigned int i;

	if (slot_input->prepared) {
		mali_c720_ctx_mgr_release_buffer_pool(ctx, pool);

		for (i = 0; i < pool->num_buffers; i++) {
			dma_free_coherent(mali_c720->dev,
					  pool->buffer_size,
					  pool->mem[i].base,
					  pool->mem[i].addr);
		}
	}

	slot_input->prepared = false;
	slot_input->sink_pad = NULL;
	slot_input->src_pad = NULL;
	slot_input->stream = 0;
}

static int mali_c720_prepare_input_port(struct mali_c720_input *input,
					struct v4l2_subdev_state *state,
					struct mali_c720_slot_input *slot_input,
					struct mali_c720_input_cfg *input_cfg)
{
	struct mali_c720 *mali_c720 = input->mdev.mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_buffer_pool *pool = &slot_input->raw_buffer_desc;
	const struct v4l2_mbus_framefmt *fmt;
	const struct mali_c720_mbus_format_info *cfg;
	struct mali_c720_buffer_request_info info;
	u32 pad = slot_input->sink_pad->index;
	u32 stream = slot_input->stream;
	unsigned int i;
	int ret;

	fmt = v4l2_subdev_state_get_format(state, pad, stream);
	if (!fmt) {
		dev_err(mali_c720->dev,
			"Failed to get format for pad %u stream %u\n",
			pad, stream);
		return -EINVAL;
	}

	cfg = mali_c720_get_mbus_config_by_code(fmt->code);
	if (!cfg) {
		dev_err(mali_c720->dev,
			"Failed to get mbus config for pad %u stream %u\n",
			pad, stream);
		return -EINVAL;
	}

	memset(&info, 0, sizeof(info));
	info.type = MALI_C720_BUF_TYPE_RAW;
	info.sub_type = MALI_C720_RAW_BUF_SUBTYPE_NORMAL;
	info.width = fmt->width;
	info.height = fmt->height;
	info.line_offset[0] = ALIGN((fmt->width * cfg->bit_depth) / 8, 32);
	info.plane_size[0] = info.line_offset[0] * info.height;
	info.num_raw_buffers = 2;
	info.buffer_cfg = cfg->buffer_cfg;

	ret = mali_c720_ctx_mgr_request_buffer_pool(ctx, &info, pool);
	if (ret)
		return ret;

	for (i = 0; i < pool->num_buffers; i++) {
		pool->mem[i].base = dma_alloc_coherent(mali_c720->dev,
						       pool->buffer_size,
						       &pool->mem[i].addr,
						       GFP_KERNEL);
		if (!pool->mem[i].base) {
			ret = -ENOMEM;
			goto release_buf_pool;
		}

		mali_c720_ctx_mgr_update_buffer(ctx, pool, &pool->mem[i].addr, i);
	}

	input_cfg->id = 0;
	input_cfg->bit_depth = cfg->bit_depth;
	input_cfg->pool_id = pool->pool_id;

	return 0;

release_buf_pool:
	mali_c720_ctx_mgr_release_buffer_pool(ctx, pool);

	while (i--) {
		dma_free_coherent(mali_c720->dev,
				  pool->buffer_size,
				  pool->mem[i].base,
				  pool->mem[i].addr);
	}

	return ret;
}

static void mali_c720_stop_inputs(struct mali_c720_slot_input *slot_input)
{
	struct v4l2_subdev *src_sd = media_entity_to_v4l2_subdev(slot_input->src_pad->entity);

	v4l2_subdev_disable_streams(src_sd, slot_input->src_pad->index,
				    BIT(slot_input->stream));
}

static int mali_c720_input_enable_streams(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *state, u32 pad,
					  u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720 *mali_c720 = mdev->mali_c720;
	struct mali_c720_context *ctx = mali_c720_get_active_context(mali_c720);
	struct mali_c720_input *input = mali_c720_sd_to_sd(mdev, struct mali_c720_input);
	struct mali_c720_input_cfg input_cfg = {0};
	struct mali_c720_slot_input *slot_input = &input->slot_input;
	struct v4l2_subdev_route *route;
	struct v4l2_subdev *src_sd;
	int ret;

	for_each_active_route(&state->routing, route) {
		if (route->source_pad != pad)
			continue;

		slot_input->sink_pad = &input->pads[route->sink_pad];
		slot_input->stream = route->sink_stream;
		break;
	}

	if (!slot_input->sink_pad) {
		dev_err(mali_c720->dev, "No active route for source pad %u\n", pad);
		ret = -EINVAL;
		goto err_free_input_ports;
	}

	slot_input->src_pad = media_pad_remote_pad_unique(slot_input->sink_pad);
	ret = PTR_ERR_OR_ZERO(slot_input->src_pad);
	if (ret) {
		dev_err(mali_c720->dev, "Input pad %u not connected\n",
			slot_input->sink_pad->index);
		goto err_free_input_ports;
	}

	ret = mali_c720_prepare_input_port(input, state, slot_input, &input_cfg);
	if (ret)
		goto err_free_input_ports;

	slot_input->prepared = true;
	src_sd = media_entity_to_v4l2_subdev(slot_input->src_pad->entity);
	ret = v4l2_subdev_enable_streams(src_sd, slot_input->src_pad->index,
					 BIT(slot_input->stream));
	if (ret && ret != -EALREADY) {
		dev_err(mali_c720->dev, "Failed to start input source\n");
		goto err_stop_sources;
	}

	mali_c720_ctx_mgr_prepare_context(ctx, &input_cfg);

	ret = mali_c720_ctx_mgr_start_context_inputs(ctx);
	if (ret)
		goto err_stop_context;

	return 0;

err_stop_context:
	mali_c720_ctx_mgr_stop_context(ctx);
	mali_c720_ctx_mgr_stop_context_inputs(ctx);
err_stop_sources:
	mali_c720_stop_inputs(slot_input);
err_free_input_ports:
	mali_c720_free_input_ports(input);

	return ret;
}

static int mali_c720_input_disable_streams(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *state, u32 pad,
					   u64 streams_mask)
{
	struct mali_c720_subdev *mdev = v4l2_sd_to_mali_c720_sd(sd);
	struct mali_c720_input *input = mali_c720_sd_to_sd(mdev, struct mali_c720_input);
	struct mali_c720_context *ctx = mali_c720_get_active_context(mdev->mali_c720);
	struct mali_c720_slot_input *slot_input = &input->slot_input;

	mali_c720_ctx_mgr_stop_context_inputs(ctx);
	mali_c720_stop_inputs(slot_input);
	mali_c720_free_input_ports(input);

	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c720_input_pad_ops = {
	.enum_mbus_code		= mali_c720_input_enum_mbus_code,
	.enum_frame_size	= mali_c720_input_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c720_input_set_fmt,
	.link_validate		= v4l2_subdev_link_validate_default,
	.enable_streams		= mali_c720_input_enable_streams,
	.disable_streams	= mali_c720_input_disable_streams,
};

static const struct v4l2_subdev_ops mali_c720_input_ops = {
	.pad	= &mali_c720_input_pad_ops,
};

static int mali_c720_input_init_state(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state)
{
	static const struct v4l2_mbus_framefmt format = {
		.width = MALI_C720_DEFAULT_WIDTH,
		.height = MALI_C720_DEFAULT_HEIGHT,
		.field = V4L2_FIELD_NONE,
		.code = MEDIA_BUS_FMT_SRGGB24_1X24,
		.colorspace = V4L2_COLORSPACE_RAW,
		.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_RAW),
		.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_RAW),
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	};
	struct v4l2_subdev_route route = {
		.sink_pad = MALI_C720_INPUT_PAD_SINK_INPUT0,
		.sink_stream = 0,
		.source_pad = MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT,
		.source_stream = 0,
		.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
	};
	struct v4l2_subdev_krouting routing = {
		.num_routes = 1,
		.routes = &route,
	};

	return v4l2_subdev_set_routing_with_fmt(sd, state, &routing, &format);
}

static const struct v4l2_subdev_internal_ops mali_c720_input_internal_ops = {
	.init_state = mali_c720_input_init_state,
};

static int mali_c720_input_subdev_link_setup(struct media_entity *entity,
					     const struct media_pad *local,
					     const struct media_pad *remote,
					     u32 flags)
{
	struct v4l2_subdev *sd = media_entity_to_v4l2_subdev(entity);
	struct mali_c720 *mali_c720 = v4l2_sd_to_mali_c720_sd(sd)->mali_c720;
	struct media_pad *remote_src;

	/*
	 * Sink and source pads support only one enabled connection.
	 */

	if (flags & MEDIA_LNK_FL_ENABLED) {
		remote_src = media_pad_remote_pad_unique(local);

		/* No previous link connected */
		if (IS_ERR(remote_src))
			return 0;

		if (remote_src != remote) {
			dev_err(mali_c720->dev,
				"Input pad %u is already connected to another pad\n",
				local->index);
			return -EBUSY;
		}
	}

	return 0;
}

static int mali_c720_input_get_fwnode_pad(struct media_entity *entity,
					  struct fwnode_endpoint *endpoint)
{
	int port;

	port = v4l2_subdev_get_fwnode_pad_1_to_1(entity, endpoint);
	if (port < 0)
		return port;
	if (port)
		return -EINVAL;

	return MALI_C720_INPUT_PAD_SINK_INPUT0;
}

static const struct media_entity_operations mali_c720_input_media_ops = {
	.link_setup		= mali_c720_input_subdev_link_setup,
	.link_validate		= v4l2_subdev_link_validate,
	.get_fwnode_pad		= mali_c720_input_get_fwnode_pad,
};

int mali_c720_register_input(struct mali_c720 *mali_c720)
{
	struct mali_c720_input *input = &mali_c720->input;
	struct mali_c720_subdev *mdev = &input->mdev;
	struct v4l2_subdev *sd = &mdev->sd;
	int ret;

	mdev->mali_c720 = mali_c720;

	v4l2_subdev_init(sd, &mali_c720_input_ops);
	sd->dev = mali_c720->dev;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	sd->entity.ops = &mali_c720_input_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->internal_ops = &mali_c720_input_internal_ops;
	strscpy(sd->name, "mali-c720 input", sizeof(sd->name));

	input->pads[MALI_C720_INPUT_PAD_SINK_INPUT0].flags =
		MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	input->pads[MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity,
				     MALI_C720_INPUT_NUM_PADS,
				     input->pads);
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

void mali_c720_unregister_input(struct mali_c720 *mali_c720)
{
	struct mali_c720_input *input = &mali_c720->input;
	struct mali_c720_subdev *mdev = &input->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_device_unregister_subdev(&mdev->sd);
}

void mali_c720_cleanup_input(struct mali_c720 *mali_c720)
{
	struct mali_c720_input *input = &mali_c720->input;
	struct mali_c720_subdev *mdev = &input->mdev;

	if (!mdev->mali_c720)
		return;

	v4l2_subdev_cleanup(&mdev->sd);
	media_entity_cleanup(&mdev->sd.entity);
	mdev->mali_c720 = NULL;
}
