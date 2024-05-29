// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Image signal processor
 *
 * Copyright (C) 2024 Ideas on Board Oy
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/property.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"

static const struct mali_c55_isp_fmt mali_c55_isp_fmts[] = {
	{
		.code = MEDIA_BUS_FMT_SRGGB20_1X20,
		.order = MALI_C55_BAYER_ORDER_RGGB,
		.encoding = V4L2_PIXEL_ENC_BAYER,
	},
	{
		.code = MEDIA_BUS_FMT_SGRBG20_1X20,
		.order = MALI_C55_BAYER_ORDER_GRBG,
		.encoding = V4L2_PIXEL_ENC_BAYER,
	},
	{
		.code = MEDIA_BUS_FMT_SGBRG20_1X20,
		.order = MALI_C55_BAYER_ORDER_GBRG,
		.encoding = V4L2_PIXEL_ENC_BAYER,
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR20_1X20,
		.order = MALI_C55_BAYER_ORDER_BGGR,
		.encoding = V4L2_PIXEL_ENC_BAYER,
	},
	{
		.code = MEDIA_BUS_FMT_RGB202020_1X60,
		.order = 0, /* Not relevant for this format */
		.encoding = V4L2_PIXEL_ENC_RGB,
	}
	/*
	 * TODO: Support MEDIA_BUS_FMT_YUV20_1X60 here. This is so that we can
	 * also support YUV input from a sensor passed-through to the output. At
	 * present we have no mechanism to test that though so it may have to
	 * wait a while...
	 */
};

const struct mali_c55_isp_fmt *
mali_c55_isp_fmt_next(const struct mali_c55_isp_fmt *fmt)
{
	if (!fmt)
		fmt = &mali_c55_isp_fmts[0];
	else
		fmt++;

	for (; fmt < &mali_c55_isp_fmts[ARRAY_SIZE(mali_c55_isp_fmts)]; fmt++)
		return fmt;

	return NULL;
}

bool mali_c55_isp_is_format_supported(unsigned int mbus_code)
{
	const struct mali_c55_isp_fmt *isp_fmt;

	for_each_mali_isp_fmt(isp_fmt) {
		if (isp_fmt->code == mbus_code)
			return true;
	}

	return false;
}

static const struct mali_c55_isp_fmt *
mali_c55_isp_get_mbus_config_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c55_isp_fmts); i++) {
		if (mali_c55_isp_fmts[i].code == code)
			return &mali_c55_isp_fmts[i];
	}

	return NULL;
}

static void mali_c55_isp_stop(struct mali_c55 *mali_c55)
{
	u32 val;

	mali_c55_write(mali_c55, MALI_C55_REG_INPUT_MODE_REQUEST, MALI_C55_INPUT_SAFE_STOP);
	readl_poll_timeout(mali_c55->base + MALI_C55_REG_MODE_STATUS,
			   val, !val, 10 * USEC_PER_MSEC, 250 * USEC_PER_MSEC);
}

static int mali_c55_isp_start(struct mali_c55 *mali_c55)
{
	struct mali_c55_ctx *ctx = mali_c55_get_active_context(mali_c55);
	const struct mali_c55_isp_fmt *cfg;
	struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	struct v4l2_rect *crop;
	struct v4l2_subdev *sd;
	u32 val;
	int ret;

	sd = &mali_c55->isp.sd;

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MCU_CONFIG,
			     MALI_C55_REG_MCU_CONFIG_WRITE_MASK, 0x01);

	/* Apply input windowing */
	state = v4l2_subdev_get_locked_active_state(sd);
	crop = v4l2_subdev_state_get_crop(state, MALI_C55_ISP_PAD_SINK_VIDEO);
	format = v4l2_subdev_state_get_format(state,
					      MALI_C55_ISP_PAD_SINK_VIDEO);
	cfg = mali_c55_isp_get_mbus_config_by_code(format->code);

	mali_c55_write(mali_c55, MALI_C55_REG_HC_START,
		       MALI_C55_HC_START(crop->left));
	mali_c55_write(mali_c55, MALI_C55_REG_HC_SIZE,
		       MALI_C55_HC_SIZE(crop->width));
	mali_c55_write(mali_c55, MALI_C55_REG_VC_START_SIZE,
		       MALI_C55_VC_START(crop->top) |
		       MALI_C55_VC_SIZE(crop->height));
	mali_c55_update_bits(mali_c55, MALI_C55_REG_BASE_ADDR,
			     MALI_C55_REG_ACTIVE_WIDTH_MASK, format->width);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_BASE_ADDR,
			     MALI_C55_REG_ACTIVE_HEIGHT_MASK, format->height);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_BAYER_ORDER,
			     MALI_C55_BAYER_ORDER_MASK, cfg->order);
	mali_c55_update_bits(mali_c55, MALI_C55_REG_INPUT_WIDTH,
			     MALI_C55_INPUT_WIDTH_MASK,
			     MALI_C55_INPUT_WIDTH_20BIT);

	mali_c55_update_bits(mali_c55, MALI_C55_REG_ISP_RAW_BYPASS,
			     MALI_C55_ISP_RAW_BYPASS_BYPASS_MASK,
			     cfg->encoding == V4L2_PIXEL_ENC_RGB ?
			     MALI_C55_ISP_RAW_BYPASS_BYPASS_MASK : 0x00);

	ret = mali_c55_config_write(ctx, MALI_C55_CONFIG_PING);
	if (ret) {
		dev_err(mali_c55->dev, "failed to DMA config\n");
		return ret;
	}

	mali_c55_write(mali_c55, MALI_C55_REG_INPUT_MODE_REQUEST,
		       MALI_C55_INPUT_SAFE_START);
	readl_poll_timeout(mali_c55->base + MALI_C55_REG_MODE_STATUS,
			   val, !val, 10 * USEC_PER_MSEC, 250 * USEC_PER_MSEC);

	return 0;
}

void mali_c55_isp_stop_stream(struct mali_c55_isp *isp)
{
	struct mali_c55 *mali_c55 = isp->mali_c55;
	struct v4l2_subdev *sd;

	if (isp->remote_src) {
		sd = media_entity_to_v4l2_subdev(isp->remote_src->entity);
		v4l2_subdev_disable_streams(sd, isp->remote_src->index, BIT(0));
	}
	isp->remote_src = NULL;

	mali_c55_isp_stop(mali_c55);
}

int mali_c55_isp_start_stream(struct mali_c55_isp *isp)
{
	struct mali_c55 *mali_c55 = isp->mali_c55;
	struct media_pad *sink_pad;
	struct v4l2_subdev *sd;
	int ret;

	sink_pad = &isp->pads[MALI_C55_ISP_PAD_SINK_VIDEO];
	isp->remote_src = media_pad_remote_pad_unique(sink_pad);
	if (IS_ERR(isp->remote_src)) {
		dev_err(mali_c55->dev, "Failed to get source for ISP\n");
		return PTR_ERR(isp->remote_src);
	}

	sd = media_entity_to_v4l2_subdev(isp->remote_src->entity);

	isp->frame_sequence = 0;
	ret = mali_c55_isp_start(mali_c55);
	if (ret) {
		dev_err(mali_c55->dev, "Failed to start ISP\n");
		isp->remote_src = NULL;
		return ret;
	}

	/*
	 * We only support a single input stream, so we can just enable the 1st
	 * entry in the streams mask.
	 */
	ret = v4l2_subdev_enable_streams(sd, isp->remote_src->index, BIT(0));
	if (ret) {
		dev_err(mali_c55->dev, "Failed to start ISP source\n");
		mali_c55_isp_stop(mali_c55);
		return ret;
	}

	return 0;
}

static int mali_c55_isp_enum_mbus_code(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_mbus_code_enum *code)
{
	/*
	 * Only the internal RGB processed format is allowed on the regular
	 * processing source pad.
	 */
	if (code->pad == MALI_C55_ISP_PAD_SOURCE) {
		if (code->index)
			return -EINVAL;

		code->code = MEDIA_BUS_FMT_RGB121212_1X36;
		return 0;
	}

	/* On the sink and bypass pads all the supported formats are allowed. */
	if (code->index >= ARRAY_SIZE(mali_c55_isp_fmts))
		return -EINVAL;

	code->code = mali_c55_isp_fmts[code->index].code;

	return 0;
}

static int mali_c55_isp_enum_frame_size(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_frame_size_enum *fse)
{
	const struct mali_c55_isp_fmt *cfg;

	if (fse->index > 0)
		return -EINVAL;

	/*
	 * Only the internal RGB processed format is allowed on the regular
	 * processing source pad.
	 *
	 * On the sink and bypass pads all the supported formats are allowed.
	 */
	if (fse->pad == MALI_C55_ISP_PAD_SOURCE) {
		if (fse->code != MEDIA_BUS_FMT_RGB121212_1X36)
			return -EINVAL;
	} else {
		cfg = mali_c55_isp_get_mbus_config_by_code(fse->code);
		if (!cfg)
			return -EINVAL;
	}

	fse->min_width = MALI_C55_MIN_WIDTH;
	fse->min_height = MALI_C55_MIN_HEIGHT;
	fse->max_width = MALI_C55_MAX_WIDTH;
	fse->max_height = MALI_C55_MAX_HEIGHT;

	return 0;
}

static int mali_c55_isp_set_fmt(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *src_fmt, *sink_fmt;
	const struct mali_c55_isp_fmt *cfg;
	struct v4l2_rect *crop;

	/*
	 * Disallow set_fmt on the source pads; format is fixed and the sizes
	 * are the result of applying the sink crop rectangle to the sink
	 * format.
	 */
	if (format->pad)
		return v4l2_subdev_get_fmt(sd, state, format);

	cfg = mali_c55_isp_get_mbus_config_by_code(fmt->code);
	if (!cfg)
		fmt->code = MEDIA_BUS_FMT_SRGGB20_1X20;
	fmt->field = V4L2_FIELD_NONE;

	/*
	 * Clamp sizes in the accepted limits and clamp the crop rectangle in
	 * the new sizes.
	 */
	clamp_t(unsigned int, fmt->width,
		MALI_C55_MIN_WIDTH, MALI_C55_MAX_WIDTH);
	clamp_t(unsigned int, fmt->width,
		MALI_C55_MIN_HEIGHT, MALI_C55_MAX_HEIGHT);

	sink_fmt = v4l2_subdev_state_get_format(state,
						MALI_C55_ISP_PAD_SINK_VIDEO);
	*sink_fmt = *fmt;

	crop = v4l2_subdev_state_get_crop(state, MALI_C55_ISP_PAD_SINK_VIDEO);
	crop->left = 0;
	crop->top = 0;
	crop->width = fmt->width;
	crop->height = fmt->height;

	/*
	 * Propagate format to source pads. On the 'regular' output pad use
	 * the internal RGB processed format, while on the bypass pad simply
	 * replicate the ISP sink format. The sizes on both pads are the same as
	 * the ISP sink crop rectangle.
	 */
	src_fmt = v4l2_subdev_state_get_format(state, MALI_C55_ISP_PAD_SOURCE);
	src_fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;
	src_fmt->width = crop->width;
	src_fmt->height = crop->height;

	src_fmt = v4l2_subdev_state_get_format(state,
					       MALI_C55_ISP_PAD_SOURCE_BYPASS);
	src_fmt->code = fmt->code;
	src_fmt->width = crop->width;
	src_fmt->height = crop->height;

	return 0;
}

static int mali_c55_isp_get_selection(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_selection *sel)
{
	if (sel->pad || sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	sel->r = *v4l2_subdev_state_get_crop(state, MALI_C55_ISP_PAD_SINK_VIDEO);

	return 0;
}

static int mali_c55_isp_set_selection(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *src_fmt;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop;

	if (sel->pad || sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	fmt = v4l2_subdev_state_get_format(state, MALI_C55_ISP_PAD_SINK_VIDEO);

	clamp_t(unsigned int, sel->r.left, 0, fmt->width);
	clamp_t(unsigned int, sel->r.top, 0, fmt->height);
	clamp_t(unsigned int, sel->r.width, MALI_C55_MIN_WIDTH,
		fmt->width - sel->r.left);
	clamp_t(unsigned int, sel->r.height, MALI_C55_MIN_HEIGHT,
		fmt->height - sel->r.top);

	crop = v4l2_subdev_state_get_crop(state, MALI_C55_ISP_PAD_SINK_VIDEO);
	*crop = sel->r;

	/* Propagate the crop rectangle sizes to the source pad format. */
	src_fmt = v4l2_subdev_state_get_format(state, MALI_C55_ISP_PAD_SOURCE);
	src_fmt->width = crop->width;
	src_fmt->height = crop->height;

	return 0;
}

static const struct v4l2_subdev_pad_ops mali_c55_isp_pad_ops = {
	.enum_mbus_code		= mali_c55_isp_enum_mbus_code,
	.enum_frame_size	= mali_c55_isp_enum_frame_size,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= mali_c55_isp_set_fmt,
	.get_selection		= mali_c55_isp_get_selection,
	.set_selection		= mali_c55_isp_set_selection,
	.link_validate		= v4l2_subdev_link_validate_default,
};

void mali_c55_isp_queue_event_sof(struct mali_c55 *mali_c55)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};

	event.u.frame_sync.frame_sequence = mali_c55->isp.frame_sequence;
	v4l2_event_queue(mali_c55->isp.sd.devnode, &event);
}

static int
mali_c55_isp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			     struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static const struct v4l2_subdev_core_ops mali_c55_isp_core_ops = {
	.subscribe_event = mali_c55_isp_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops mali_c55_isp_ops = {
	.pad	= &mali_c55_isp_pad_ops,
	.core	= &mali_c55_isp_core_ops,
};

static int mali_c55_isp_init_state(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	struct v4l2_rect *in_crop;

	sink_fmt = v4l2_subdev_state_get_format(sd_state,
						MALI_C55_ISP_PAD_SINK_VIDEO);
	src_fmt = v4l2_subdev_state_get_format(sd_state,
					       MALI_C55_ISP_PAD_SOURCE);
	in_crop = v4l2_subdev_state_get_crop(sd_state,
					     MALI_C55_ISP_PAD_SINK_VIDEO);

	sink_fmt->width = MALI_C55_DEFAULT_WIDTH;
	sink_fmt->height = MALI_C55_DEFAULT_HEIGHT;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = MEDIA_BUS_FMT_SRGGB20_1X20;

	*v4l2_subdev_state_get_format(sd_state,
			      MALI_C55_ISP_PAD_SOURCE_BYPASS) = *sink_fmt;

	src_fmt->width = MALI_C55_DEFAULT_WIDTH;
	src_fmt->height = MALI_C55_DEFAULT_HEIGHT;
	src_fmt->field = V4L2_FIELD_NONE;
	src_fmt->code = MEDIA_BUS_FMT_RGB121212_1X36;

	in_crop->top = 0;
	in_crop->left = 0;
	in_crop->width = MALI_C55_DEFAULT_WIDTH;
	in_crop->height = MALI_C55_DEFAULT_HEIGHT;

	return 0;
}

static const struct v4l2_subdev_internal_ops mali_c55_isp_internal_ops = {
	.init_state = mali_c55_isp_init_state,
};

static const struct media_entity_operations mali_c55_isp_media_ops = {
	.link_validate		= v4l2_subdev_link_validate,
};

static int mali_c55_isp_notifier_bound(struct v4l2_async_notifier *notifier,
				       struct v4l2_subdev *subdev,
				       struct v4l2_async_connection *asc)
{
	struct mali_c55_isp *isp = container_of(notifier,
						struct mali_c55_isp, notifier);
	struct mali_c55 *mali_c55 = isp->mali_c55;
	struct media_pad *pad = &isp->pads[MALI_C55_ISP_PAD_SINK_VIDEO];
	int ret;

	/*
	 * By default we'll flag this link enabled and the TPG disabled, but
	 * no immutable flag because we need to be able to switch between the
	 * two.
	 */
	ret = v4l2_create_fwnode_links_to_pad(subdev, pad,
					      MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(mali_c55->dev, "failed to create link for %s\n",
			subdev->name);

	return ret;
}

static int mali_c55_isp_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct mali_c55_isp *isp = container_of(notifier,
						struct mali_c55_isp, notifier);
	struct mali_c55 *mali_c55 = isp->mali_c55;

	return v4l2_device_register_subdev_nodes(&mali_c55->v4l2_dev);
}

static const struct v4l2_async_notifier_operations mali_c55_isp_notifier_ops = {
	.bound = mali_c55_isp_notifier_bound,
	.complete = mali_c55_isp_notifier_complete,
};

static int mali_c55_isp_parse_endpoint(struct mali_c55_isp *isp)
{
	struct mali_c55 *mali_c55 = isp->mali_c55;
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	v4l2_async_nf_init(&isp->notifier, &mali_c55->v4l2_dev);

	/*
	 * The ISP should have a single endpoint pointing to some flavour of
	 * CSI-2 receiver...but for now at least we do want everything to work
	 * normally even with no sensors connected, as we have the TPG. If we
	 * don't find a sensor just warn and return success.
	 */
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(mali_c55->dev),
					     0, 0, 0);
	if (!ep) {
		dev_warn(mali_c55->dev, "no local endpoint found\n");
		return 0;
	}

	asc = v4l2_async_nf_add_fwnode_remote(&isp->notifier, ep,
					      struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		dev_err(mali_c55->dev, "failed to add remote fwnode\n");
		ret = PTR_ERR(asc);
		goto err_put_ep;
	}

	isp->notifier.ops = &mali_c55_isp_notifier_ops;
	ret = v4l2_async_nf_register(&isp->notifier);
	if (ret) {
		dev_err(mali_c55->dev, "failed to register notifier\n");
		goto err_cleanup_nf;
	}

	fwnode_handle_put(ep);

	return 0;

err_cleanup_nf:
	v4l2_async_nf_cleanup(&isp->notifier);
err_put_ep:
	fwnode_handle_put(ep);

	return ret;
}

int mali_c55_register_isp(struct mali_c55 *mali_c55)
{
	struct mali_c55_isp *isp = &mali_c55->isp;
	struct v4l2_subdev *sd = &isp->sd;
	int ret;

	isp->mali_c55 = mali_c55;

	v4l2_subdev_init(sd, &mali_c55_isp_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->entity.ops = &mali_c55_isp_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->internal_ops = &mali_c55_isp_internal_ops;
	strscpy(sd->name, MALI_C55_DRIVER_NAME " isp", sizeof(sd->name));

	isp->pads[MALI_C55_ISP_PAD_SINK_VIDEO].flags = MEDIA_PAD_FL_SINK;
	isp->pads[MALI_C55_ISP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	isp->pads[MALI_C55_ISP_PAD_SOURCE_BYPASS].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, MALI_C55_ISP_NUM_PADS,
				     isp->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_cleanup_media_entity;

	ret = v4l2_device_register_subdev(&mali_c55->v4l2_dev, sd);
	if (ret)
		goto err_cleanup_subdev;

	ret = mali_c55_isp_parse_endpoint(isp);
	if (ret)
		goto err_cleanup_subdev;

	mutex_init(&isp->lock);

	return 0;

err_cleanup_subdev:
	v4l2_subdev_cleanup(sd);
err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
	isp->mali_c55 = NULL;

	return ret;
}

void mali_c55_unregister_isp(struct mali_c55 *mali_c55)
{
	struct mali_c55_isp *isp = &mali_c55->isp;

	if (!isp->mali_c55)
		return;

	mutex_destroy(&isp->lock);
	v4l2_async_nf_unregister(&isp->notifier);
	v4l2_async_nf_cleanup(&isp->notifier);
	v4l2_device_unregister_subdev(&isp->sd);
	v4l2_subdev_cleanup(&isp->sd);
	media_entity_cleanup(&isp->sd.entity);
}
