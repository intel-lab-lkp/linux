// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 NXP
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

/* CSI Pixel Formatter registers map */

#define CSI_VC_INTERLACED_LINE_CNT(vc)		(0x00 + (vc) * 0x04)
#define INTERLACED_ODD_LINE_CNT_SET(x)		FIELD_PREP(GENMASK(13, 0), (x))
#define INTERLACED_EVEN_LINE_CNT_SET(x)		FIELD_PREP(GENMASK(29, 16), (x))

#define CSI_VC_INTERLACED_CTRL			0x20

#define CSI_VC_INTERLACED_ERR			0x24
#define CSI_VC_ERR_MASK				GENMASK(7, 0)
#define CSI_VC_ERR(vc)				BIT((vc))

#define CSI_VC_YUV420_FIRST_LINE_EVEN		0x28
#define YUV420_FIRST_LINE_EVEN(vc)		BIT((vc))

#define CSI_RAW32_CTRL				0x30
#define CSI_VC_RAW32_MODE(vc)			BIT((vc))
#define CSI_VC_RAW32_SWAP_MODE(vc)		BIT((vc) + 8)

#define CSI_STREAM_FENCING_CTRL			0x34
#define CSI_VC_STREAM_FENCING(vc)		BIT((vc))
#define CSI_VC_STREAM_FENCING_RST(vc)		BIT((vc) + 8)

#define CSI_STREAM_FENCING_STS			0x38
#define CSI_STREAM_FENCING_STS_MASK		GENMASK(7, 0)

#define CSI_VC_NON_PIXEL_DATA_TYPE(vc)		(0x40 + (vc) * 0x04)

#define CSI_VC_PIXEL_DATA_CTRL(vc)		(0x60 + (vc) * 0x04)
#define NEW_VC(vc)				FIELD_PREP(GENMASK(3, 1), vc)
#define REROUTE_VC_ENABLE			BIT(0)

#define CSI_VC_ROUTE_PIXEL_DATA_TYPE(vc)	(0x80 + (vc) * 0x04)

#define CSI_VC_NON_PIXEL_DATA_CTRL(vc)		(0xa0 + (vc) * 0x04)

#define CSI_VC_PIXEL_DATA_TYPE(vc)		(0xc0 + (vc) * 0x04)

#define CSI_VC_PIXEL_DATA_TYPE_ERR(vc)		(0xe0 + (vc) * 0x04)

#define CSI_FORMATTER_PAD_SINK			0
#define CSI_FORMATTER_PAD_SOURCE		1
#define CSI_FORMATTER_PAD_NUM			2

#define CSI_FORMATTER_VC_NUM			8 /* Number of virtual channels */

struct csi_formatter_pix_format {
	u32 code;
	u32 data_type;
};

struct csi_formatter {
	struct device *dev;
	struct regmap *regs;
	struct clk *clk;

	struct v4l2_subdev sd;
	struct v4l2_subdev *csi_sd;
	struct v4l2_async_notifier notifier;
	struct media_pad pads[CSI_FORMATTER_PAD_NUM];

	u32 remote_pad;
	u32 reg_offset;

	/* Protects enabled_streams */
	struct mutex lock;
	u64 enabled_streams;
};

struct csi_formatter_dt_index {
	u8 dtype;
	u8 index;
};

/*
 * The index corresponds to the bit index in the register that enables
 * the data type of pixel data transported by the Formatter.
 */
static const struct csi_formatter_dt_index formatter_dt_to_index_map[] = {
	{ .dtype = MIPI_CSI2_DT_YUV420_8B,        .index = 0 },
	{ .dtype = MIPI_CSI2_DT_YUV420_8B_LEGACY, .index = 2 },
	{ .dtype = MIPI_CSI2_DT_YUV422_8B,        .index = 6 },
	{ .dtype = MIPI_CSI2_DT_RGB444,		  .index = 8 },
	{ .dtype = MIPI_CSI2_DT_RGB555,           .index = 9 },
	{ .dtype = MIPI_CSI2_DT_RGB565,           .index = 10 },
	{ .dtype = MIPI_CSI2_DT_RGB666,           .index = 11 },
	{ .dtype = MIPI_CSI2_DT_RGB888,           .index = 12 },
	{ .dtype = MIPI_CSI2_DT_RAW6,             .index = 16 },
	{ .dtype = MIPI_CSI2_DT_RAW7,             .index = 17 },
	{ .dtype = MIPI_CSI2_DT_RAW8,             .index = 18 },
	{ .dtype = MIPI_CSI2_DT_RAW10,            .index = 19 },
	{ .dtype = MIPI_CSI2_DT_RAW12,            .index = 20 },
	{ .dtype = MIPI_CSI2_DT_RAW14,            .index = 21 },
	{ .dtype = MIPI_CSI2_DT_RAW16,            .index = 22 },
};

static const struct csi_formatter_pix_format formats[] = {
	/* YUV formats */
	{ MEDIA_BUS_FMT_UYVY8_1X16,	MIPI_CSI2_DT_YUV422_8B },
	/* RGB formats */
	{ MEDIA_BUS_FMT_RGB565_1X16,	MIPI_CSI2_DT_RGB565 },
	{ MEDIA_BUS_FMT_RGB888_1X24,	MIPI_CSI2_DT_RGB888 },
	/* RAW (Bayer and greyscale) formats */
	{ MEDIA_BUS_FMT_SBGGR8_1X8,	MIPI_CSI2_DT_RAW8 },
	{ MEDIA_BUS_FMT_SGBRG8_1X8,	MIPI_CSI2_DT_RAW8 },
	{ MEDIA_BUS_FMT_SGRBG8_1X8,	MIPI_CSI2_DT_RAW8 },
	{ MEDIA_BUS_FMT_SRGGB8_1X8,	MIPI_CSI2_DT_RAW8 },
	{ MEDIA_BUS_FMT_Y8_1X8,		MIPI_CSI2_DT_RAW8 },
	{ MEDIA_BUS_FMT_SBGGR10_1X10,	MIPI_CSI2_DT_RAW10 },
	{ MEDIA_BUS_FMT_SGBRG10_1X10,	MIPI_CSI2_DT_RAW10 },
	{ MEDIA_BUS_FMT_SGRBG10_1X10,	MIPI_CSI2_DT_RAW10 },
	{ MEDIA_BUS_FMT_SRGGB10_1X10,	MIPI_CSI2_DT_RAW10 },
	{ MEDIA_BUS_FMT_Y10_1X10,	MIPI_CSI2_DT_RAW10 },
	{ MEDIA_BUS_FMT_SBGGR12_1X12,	MIPI_CSI2_DT_RAW12 },
	{ MEDIA_BUS_FMT_SGBRG12_1X12,	MIPI_CSI2_DT_RAW12 },
	{ MEDIA_BUS_FMT_SGRBG12_1X12,	MIPI_CSI2_DT_RAW12 },
	{ MEDIA_BUS_FMT_SRGGB12_1X12,	MIPI_CSI2_DT_RAW12 },
	{ MEDIA_BUS_FMT_Y12_1X12,	MIPI_CSI2_DT_RAW12 },
	{ MEDIA_BUS_FMT_SBGGR14_1X14,	MIPI_CSI2_DT_RAW14 },
	{ MEDIA_BUS_FMT_SGBRG14_1X14,	MIPI_CSI2_DT_RAW14 },
	{ MEDIA_BUS_FMT_SGRBG14_1X14,	MIPI_CSI2_DT_RAW14 },
	{ MEDIA_BUS_FMT_SRGGB14_1X14,	MIPI_CSI2_DT_RAW14 },
	{ MEDIA_BUS_FMT_SBGGR16_1X16,	MIPI_CSI2_DT_RAW16 },
	{ MEDIA_BUS_FMT_SGBRG16_1X16,	MIPI_CSI2_DT_RAW16 },
	{ MEDIA_BUS_FMT_SGRBG16_1X16,	MIPI_CSI2_DT_RAW16 },
	{ MEDIA_BUS_FMT_SRGGB16_1X16,	MIPI_CSI2_DT_RAW16 },
};

static const struct v4l2_mbus_framefmt formatter_default_fmt = {
	.code = MEDIA_BUS_FMT_UYVY8_1X16,
	.width = 1920U,
	.height = 1080U,
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_SMPTE170M,
	.xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(V4L2_COLORSPACE_SMPTE170M),
	.ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(V4L2_COLORSPACE_SMPTE170M),
	.quantization = V4L2_QUANTIZATION_LIM_RANGE,
};

static const struct csi_formatter_pix_format *csi_formatter_find_format(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); i++)
		if (code == formats[i].code)
			return &formats[i];

	return NULL;
}

/* -----------------------------------------------------------------------------
 * V4L2 subdev operations
 */

static inline struct csi_formatter *sd_to_formatter(struct v4l2_subdev *sdev)
{
	return container_of(sdev, struct csi_formatter, sd);
}

static int __csi_formatter_subdev_set_routing(struct v4l2_subdev *sd,
					      struct v4l2_subdev_state *state,
					      struct v4l2_subdev_krouting *routing)
{
	int ret;

	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1);
	if (ret)
		return ret;

	return v4l2_subdev_set_routing_with_fmt(sd, state, routing,
						&formatter_default_fmt);
}

static int csi_formatter_subdev_init_state(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = CSI_FORMATTER_PAD_SINK,
			.sink_stream = 0,
			.source_pad = CSI_FORMATTER_PAD_SOURCE,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		},
	};

	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	return __csi_formatter_subdev_set_routing(sd, sd_state, &routing);
}

static int csi_formatter_subdev_enum_mbus_code(struct v4l2_subdev *sd,
					       struct v4l2_subdev_state *sd_state,
					       struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad == CSI_FORMATTER_PAD_SOURCE) {
		struct v4l2_mbus_framefmt *fmt;

		if (code->index > 0)
			return -EINVAL;

		fmt = v4l2_subdev_state_get_format(sd_state, code->pad,
						   code->stream);
		code->code = fmt->code;
		return 0;
	}

	if (code->index >= ARRAY_SIZE(formats))
		return -EINVAL;

	code->code = formats[code->index].code;

	return 0;
}

static int csi_formatter_subdev_set_fmt(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_format *sdformat)
{
	struct csi_formatter_pix_format const *format;
	struct v4l2_mbus_framefmt *fmt;

	if (sdformat->pad == CSI_FORMATTER_PAD_SOURCE)
		return v4l2_subdev_get_fmt(sd, sd_state, sdformat);

	format = csi_formatter_find_format(sdformat->format.code);
	if (!format)
		format = &formats[0];

	v4l_bound_align_image(&sdformat->format.width, 1, 0xffff, 2,
			      &sdformat->format.height, 1, 0xffff, 0, 0);

	fmt = v4l2_subdev_state_get_format(sd_state, sdformat->pad,
					   sdformat->stream);
	*fmt = sdformat->format;

	/* Set default code if user set an invalid value */
	fmt->code = format->code;

	/* Propagate the format from sink stream to source stream */
	fmt = v4l2_subdev_state_get_opposite_stream_format(sd_state, sdformat->pad,
							   sdformat->stream);
	if (!fmt)
		return -EINVAL;

	*fmt = sdformat->format;

	return 0;
}

static int csi_formatter_subdev_set_routing(struct v4l2_subdev *sd,
					    struct v4l2_subdev_state *state,
					    enum v4l2_subdev_format_whence which,
					    struct v4l2_subdev_krouting *routing)
{
	if (which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    media_entity_is_streaming(&sd->entity))
		return -EBUSY;

	return __csi_formatter_subdev_set_routing(sd, state, routing);
}

static inline void csi_formatter_write(struct csi_formatter *formatter,
				       unsigned int reg, unsigned int value)
{
	u32 offset = formatter->reg_offset;

	regmap_write(formatter->regs, reg + offset, value);
}

static u8 csi_formatter_get_index_by_dt(u8 data_type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formatter_dt_to_index_map); ++i) {
		const struct csi_formatter_dt_index *entry =
			&formatter_dt_to_index_map[i];

		if (data_type == entry->dtype)
			return entry->index;
	}

	pr_warn_once("Unsupported data type 0x%x, using default\n", data_type);

	return formatter_dt_to_index_map[0].index;
}

static int csi_formatter_get_vc(struct csi_formatter *formatter,
				struct v4l2_mbus_frame_desc *fd,
				unsigned int stream)
{
	struct v4l2_mbus_frame_desc_entry *entry = NULL;
	unsigned int i;
	int vc;

	for (i = 0; i < fd->num_entries; ++i) {
		if (fd->entry[i].stream == stream) {
			entry = &fd->entry[i];
			break;
		}
	}

	if (!entry) {
		dev_err(formatter->dev,
			"No frame desc entry for stream %u\n", stream);
		return -EPIPE;
	}

	vc = entry->bus.csi2.vc;

	if (vc < 0 || vc >= CSI_FORMATTER_VC_NUM) {
		dev_err(formatter->dev, "Invalid virtual channel %d\n", vc);
		return -EINVAL;
	}

	return vc;
}

static void csi_formatter_stop_stream(struct csi_formatter *formatter,
				      u64 stream_mask)
{
	unsigned int i;

	for (i = 0; i < CSI_FORMATTER_VC_NUM; ++i) {
		if (!(stream_mask & BIT(i)))
			continue;

		csi_formatter_write(formatter, CSI_VC_PIXEL_DATA_TYPE(i), 0);
	}
}

static int csi_formatter_start_stream(struct csi_formatter *formatter,
				      struct v4l2_subdev_state *state,
				      u64 stream_mask)
{
	const struct csi_formatter_pix_format *pix_fmt;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_mbus_frame_desc fd = {};
	u64 configured_streams = 0;
	unsigned int i;
	u32 val;
	int vc;
	int ret;

	ret = v4l2_subdev_call(formatter->csi_sd, pad, get_frame_desc,
			       formatter->remote_pad, &fd);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		dev_err(formatter->dev, "Failed to get frame desc: %d\n", ret);
		return ret;
	}

	for (i = 0; i < CSI_FORMATTER_VC_NUM; ++i) {
		if (!(stream_mask & BIT(i)))
			continue;

		fmt = v4l2_subdev_state_get_format(state,
						   CSI_FORMATTER_PAD_SINK, i);

		pix_fmt = csi_formatter_find_format(fmt->code);

		val = BIT(csi_formatter_get_index_by_dt(pix_fmt->data_type));

		if (ret == -ENOIOCTLCMD) {
			/*
			 * Source doesn't implement get_frame_desc, use
			 * default VC 0
			 */
			vc = 0;
		} else {
			vc = csi_formatter_get_vc(formatter, &fd, i);
			if (vc < 0) {
				ret = vc;
				goto err_cleanup;
			}
		}

		csi_formatter_write(formatter, CSI_VC_PIXEL_DATA_TYPE(vc), val);
		configured_streams |= BIT(i);
	}

	return 0;

err_cleanup:
	csi_formatter_stop_stream(formatter, configured_streams);
	return ret;
}

static int csi_formatter_subdev_enable_streams(struct v4l2_subdev *sd,
					       struct v4l2_subdev_state *state,
					       u32 pad, u64 streams_mask)
{
	struct csi_formatter *formatter = sd_to_formatter(sd);
	struct device *dev = formatter->dev;
	u64 sink_streams;
	int ret;

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       CSI_FORMATTER_PAD_SOURCE,
						       CSI_FORMATTER_PAD_SINK,
						       &streams_mask);
	if (!sink_streams || !streams_mask)
		return -EINVAL;

	guard(mutex)(&formatter->lock);

	if (!formatter->enabled_streams) {
		ret = pm_runtime_resume_and_get(formatter->dev);
		if (ret < 0) {
			dev_err(dev, "Failed to resume runtime PM: %d\n", ret);
			return ret;
		}
	}

	ret = csi_formatter_start_stream(formatter, state, streams_mask);
	if (ret)
		goto err_runtime_put;

	ret = v4l2_subdev_enable_streams(formatter->csi_sd,
					 formatter->remote_pad,
					 sink_streams);
	if (ret)
		goto err_stop_stream;

	formatter->enabled_streams |= streams_mask;

	return 0;

err_stop_stream:
	csi_formatter_stop_stream(formatter, streams_mask);
err_runtime_put:
	if (!formatter->enabled_streams)
		pm_runtime_put(formatter->dev);
	return ret;
}

static int csi_formatter_subdev_disable_streams(struct v4l2_subdev *sd,
						struct v4l2_subdev_state *state,
						u32 pad, u64 streams_mask)
{
	struct csi_formatter *formatter = sd_to_formatter(sd);
	u64 sink_streams;
	int ret;

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       CSI_FORMATTER_PAD_SOURCE,
						       CSI_FORMATTER_PAD_SINK,
						       &streams_mask);
	if (!sink_streams || !streams_mask)
		return -EINVAL;

	guard(mutex)(&formatter->lock);

	ret = v4l2_subdev_disable_streams(formatter->csi_sd, formatter->remote_pad,
					  sink_streams);
	if (ret)
		dev_err(formatter->dev, "Failed to disable streams: %d\n", ret);

	csi_formatter_stop_stream(formatter, streams_mask);

	formatter->enabled_streams &= ~streams_mask;

	if (!formatter->enabled_streams)
		pm_runtime_put(formatter->dev);

	return ret;
}

static const struct v4l2_subdev_pad_ops formatter_subdev_pad_ops = {
	.enum_mbus_code		= csi_formatter_subdev_enum_mbus_code,
	.get_fmt		= v4l2_subdev_get_fmt,
	.set_fmt		= csi_formatter_subdev_set_fmt,
	.get_frame_desc		= v4l2_subdev_get_frame_desc_passthrough,
	.set_routing		= csi_formatter_subdev_set_routing,
	.enable_streams		= csi_formatter_subdev_enable_streams,
	.disable_streams	= csi_formatter_subdev_disable_streams,
};

static const struct v4l2_subdev_ops formatter_subdev_ops = {
	.pad = &formatter_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops formatter_internal_ops = {
	.init_state = csi_formatter_subdev_init_state,
};

/* -----------------------------------------------------------------------------
 * Media entity operations
 */

static const struct media_entity_operations formatter_entity_ops = {
	.link_validate	= v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

static int csi_formatter_subdev_init(struct csi_formatter *formatter)
{
	struct v4l2_subdev *sd = &formatter->sd;
	int ret;

	v4l2_subdev_init(sd, &formatter_subdev_ops);

	snprintf(sd->name, sizeof(sd->name), "%s", dev_name(formatter->dev));
	sd->internal_ops = &formatter_internal_ops;

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS |
		     V4L2_SUBDEV_FL_STREAMS;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	sd->entity.ops = &formatter_entity_ops;
	sd->dev = formatter->dev;

	formatter->pads[CSI_FORMATTER_PAD_SINK].flags = MEDIA_PAD_FL_SINK
						      | MEDIA_PAD_FL_MUST_CONNECT;
	formatter->pads[CSI_FORMATTER_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, CSI_FORMATTER_PAD_NUM,
				     formatter->pads);
	if (ret) {
		dev_err(formatter->dev, "Failed to init pads\n");
		return ret;
	}

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		media_entity_cleanup(&sd->entity);

	return ret;
}

static inline struct csi_formatter *
notifier_to_csi_formatter(struct v4l2_async_notifier *n)
{
	return container_of(n, struct csi_formatter, notifier);
}

static int csi_formatter_notify_bound(struct v4l2_async_notifier *notifier,
				      struct v4l2_subdev *sd,
				      struct v4l2_async_connection *asc)
{
	const unsigned int link_flags = MEDIA_LNK_FL_IMMUTABLE
				      | MEDIA_LNK_FL_ENABLED;
	struct csi_formatter *formatter = notifier_to_csi_formatter(notifier);
	struct v4l2_subdev *sdev = &formatter->sd;
	struct media_pad *sink = &sdev->entity.pads[CSI_FORMATTER_PAD_SINK];
	struct media_pad *remote_pad;
	int ret;

	formatter->csi_sd = sd;

	dev_dbg(formatter->dev, "Bound subdev: %s pad\n", sd->name);

	ret = v4l2_create_fwnode_links_to_pad(sd, sink, link_flags);
	if (ret < 0)
		return ret;

	remote_pad = media_pad_remote_pad_first(sink);
	if (!remote_pad) {
		dev_err(formatter->dev, "Pipe not setup correctly\n");
		return -EPIPE;
	}
	formatter->remote_pad = remote_pad->index;

	return 0;
}

static const struct v4l2_async_notifier_operations formatter_notify_ops = {
	.bound = csi_formatter_notify_bound,
};

static int csi_formatter_async_register(struct csi_formatter *formatter)
{
	struct device *dev = formatter->dev;
	struct v4l2_async_connection *asc;
	int ret;

	struct fwnode_handle *ep __free(fwnode_handle) =
		fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return -ENOTCONN;

	v4l2_async_subdev_nf_init(&formatter->notifier, &formatter->sd);

	asc = v4l2_async_nf_add_fwnode_remote(&formatter->notifier, ep,
					      struct v4l2_async_connection);
	if (IS_ERR(asc)) {
		ret = PTR_ERR(asc);
		goto err_cleanup_notifier;
	}

	formatter->notifier.ops = &formatter_notify_ops;

	ret = v4l2_async_nf_register(&formatter->notifier);
	if (ret)
		goto err_cleanup_notifier;

	ret = v4l2_async_register_subdev(&formatter->sd);
	if (ret)
		goto err_unregister_notifier;

	return 0;

err_unregister_notifier:
	v4l2_async_nf_unregister(&formatter->notifier);
err_cleanup_notifier:
	v4l2_async_nf_cleanup(&formatter->notifier);
	return ret;
}

static void csi_formatter_async_unregister(struct csi_formatter *formatter)
{
	v4l2_async_unregister_subdev(&formatter->sd);
	v4l2_async_nf_unregister(&formatter->notifier);
	v4l2_async_nf_cleanup(&formatter->notifier);
}

/* -----------------------------------------------------------------------------
 * Suspend/resume
 */

static int csi_formatter_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct csi_formatter *formatter = sd_to_formatter(sd);

	clk_disable_unprepare(formatter->clk);

	return 0;
}

static int csi_formatter_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct csi_formatter *formatter = sd_to_formatter(sd);

	return clk_prepare_enable(formatter->clk);
}

static DEFINE_RUNTIME_DEV_PM_OPS(csi_formatter_pm_ops,
				 csi_formatter_runtime_suspend,
				 csi_formatter_runtime_resume, NULL);

static int csi_formatter_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct csi_formatter *formatter;
	u32 val;
	int ret;

	formatter = devm_kzalloc(dev, sizeof(*formatter), GFP_KERNEL);
	if (!formatter)
		return -ENOMEM;

	formatter->dev = dev;

	ret = devm_mutex_init(dev, &formatter->lock);
	if (ret)
		return ret;

	formatter->regs = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(formatter->regs))
		return dev_err_probe(dev, PTR_ERR(formatter->regs),
				     "Failed to get csi formatter regmap\n");

	ret = of_property_read_u32(dev->of_node, "reg", &val);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get csi formatter reg property\n");

	formatter->reg_offset = val;

	formatter->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(formatter->clk))
		return dev_err_probe(dev, PTR_ERR(formatter->clk),
				     "Failed to get pixel clock\n");

	ret = csi_formatter_subdev_init(formatter);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to initialize formatter subdev\n");

	platform_set_drvdata(pdev, &formatter->sd);

	/* Enable runtime PM. */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_cleanup_subdev;

	ret = csi_formatter_async_register(formatter);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to register async subdevice\n");
		goto err_cleanup_subdev;
	}

	return 0;

err_cleanup_subdev:
	v4l2_subdev_cleanup(&formatter->sd);
	media_entity_cleanup(&formatter->sd.entity);
	return ret;
}

static void csi_formatter_remove(struct platform_device *pdev)
{
	struct v4l2_subdev *sd = platform_get_drvdata(pdev);
	struct csi_formatter *formatter = sd_to_formatter(sd);

	csi_formatter_async_unregister(formatter);

	v4l2_subdev_cleanup(&formatter->sd);
	media_entity_cleanup(&formatter->sd.entity);
}

static const struct of_device_id csi_formatter_of_match[] = {
	{ .compatible = "fsl,imx95-csi-formatter" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, csi_formatter_of_match);

static struct platform_driver csi_formatter_device_driver = {
	.driver = {
		.name           = "csi-pixel-formatter",
		.of_match_table = csi_formatter_of_match,
		.pm             = pm_ptr(&csi_formatter_pm_ops),
	},
	.probe  = csi_formatter_probe,
	.remove = csi_formatter_remove,
};

module_platform_driver(csi_formatter_device_driver);

MODULE_AUTHOR("NXP Semiconductor, Inc.");
MODULE_DESCRIPTION("NXP i.MX95 CSI Pixel Formatter driver");
MODULE_LICENSE("GPL");
