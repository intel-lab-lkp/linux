// SPDX-License-Identifier: GPL-2.0
/*
 * Maxim GMSL2 Deserializer Driver
 *
 * Copyright (C) 2023 Collabora Ltd.
 */

#include <linux/bitops.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define MAX96714F_DEVICE_ID 0xca
#define MAX96714_NPORTS     2
#define MAX96714_PAD_SINK   0
#define MAX96714_PAD_SOURCE 1

/* DEV */
#define MAX96714_REG13         0x0d
#define MAX96714_DEV_REV       0x0e
#define MAX96714_DEV_REV_MASK  GENMASK(3, 0)
#define MAX96714_LINK_LOCK     0x13
#define MAX96714_LINK_LOCK_BIT BIT(3)

/* VID_RX */
#define MAX96714_VIDEO_RX8 0x11a
#define MAX96714_VID_LOCK  BIT(6)

/* BACKTOP */
#define MAX96714_BACKTOP25 0x320
#define CSI_DPLL_FREQ_MASK GENMASK(4, 0)

/* MIPI_PHY */
#define MAX96714_MIPI_STDBY_N    0x332
#define MAX96714_MIPI_STDBY_MASK GENMASK(5, 4)
#define MAX96714_MIPI_LANE_MAP   0x333
#define MAX96714_MIPI_POLARITY   0x335
#define MAX96714_MIPI_POLARITY_MASK GENMASK(5, 0)

/* MIPI_TX */
#define MAX96714_MIPI_LANE_CNT 0x44a
#define MAX96714_CSI2_LANE_CNT_MASK GENMASK(7, 6)
#define MAX96714_MIPI_TX52 0x474
#define MAX96714_TUN_EN BIT(0)

#define MHZ(v) ((u32)((v)  * 1000000U))

struct max96714_rxport {
	struct {
		struct v4l2_subdev   *sd;
		u16 pad;
		struct fwnode_handle *ep_fwnode;
	} source;
	struct regulator	     *poc;
};

struct max96714_txport {
	struct v4l2_fwnode_endpoint vep;
};

struct max96714_priv {
	struct i2c_client *client;
	struct regmap     *regmap;
	struct gpio_desc  *gpiod_pwdn;
	struct max96714_rxport rxport;
	struct i2c_mux_core *mux;
	u64 enabled_source_streams;
	struct v4l2_subdev	sd;
	struct media_pad	pads[MAX96714_NPORTS];
	struct v4l2_fwnode_endpoint vep;
	struct v4l2_ctrl_handler   ctrl_handler;
	struct v4l2_async_notifier notifier;
	s64 tx_link_freq;
};

static struct regmap_config max96714_regmap_config = {
	.name = "max96714",
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xFFFF,
};

static inline struct max96714_priv *sd_to_max96714(struct v4l2_subdev *sd)
{
	return container_of(sd, struct max96714_priv, sd);
}

static int max96714_read(struct max96714_priv *priv, u16 reg, u8 *val)
{
	struct device *dev = &priv->client->dev;
	unsigned int v;
	int ret;

	ret = regmap_read(priv->regmap, reg, &v);
	if (ret) {
		dev_err(dev, "%s: cannot read register 0x%02x (%d)!\n",
			__func__, reg, ret);
	}

	*val = v;

	return ret;
}

static int max96714_write(struct max96714_priv *priv, u16 reg, u8 val)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		dev_err(dev, "%s: cannot write register 0x%02x (%d)!\n",
			__func__, reg, ret);

	return ret;
}

static int max96714_update_bits(struct max96714_priv *priv, u16 reg, u8 mask,
				u8 val)
{
	struct device *dev = &priv->client->dev;
	int ret;

	ret = regmap_update_bits(priv->regmap, reg, mask, val);
	if (ret)
		dev_err(dev, "%s: cannot update register 0x%02x (%d)!\n",
			__func__, reg, ret);

	return ret;
}

static int max96714_enable_tx_port(struct max96714_priv *priv)
{
	return max96714_update_bits(priv, MAX96714_MIPI_STDBY_N,
				    MAX96714_MIPI_STDBY_MASK,
				    MAX96714_MIPI_STDBY_MASK);
}

static void max96714_disable_tx_port(struct max96714_priv *priv)
{
	max96714_update_bits(priv, MAX96714_MIPI_STDBY_N,
			     MAX96714_MIPI_STDBY_MASK, 0);
}

static bool max96714_tx_port_enabled(struct max96714_priv *priv)
{
	u8 val;

	max96714_read(priv, MAX96714_MIPI_STDBY_N, &val);

	return val & MAX96714_MIPI_STDBY_MASK;
}

static int max96714_enable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   u32 source_pad, u64 streams_mask)
{
	struct max96714_priv *priv = sd_to_max96714(sd);
	u64 sink_streams;
	int ret;

	if (!priv->enabled_source_streams)
		max96714_enable_tx_port(priv);

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       MAX96714_PAD_SOURCE,
						       MAX96714_PAD_SINK,
						       &streams_mask);

	ret = v4l2_subdev_enable_streams(priv->rxport.source.sd,
					 priv->rxport.source.pad,
					 sink_streams);
	if (ret)
		return ret;

	priv->enabled_source_streams |= streams_mask;


	return ret;
}

static int max96714_disable_streams(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    u32 source_pad, u64 streams_mask)
{
	struct max96714_priv *priv = sd_to_max96714(sd);
	u64 sink_streams;
	int ret;

	sink_streams = v4l2_subdev_state_xlate_streams(state,
						       MAX96714_PAD_SOURCE,
						       MAX96714_PAD_SINK,
						       &streams_mask);

	ret = v4l2_subdev_disable_streams(priv->rxport.source.sd,
					  priv->rxport.source.pad,
					  sink_streams);
	if (ret)
		return ret;

	priv->enabled_source_streams &= ~streams_mask;

	if (!priv->enabled_source_streams)
		max96714_disable_tx_port(priv);

	return 0;
}

static int max96714_s_stream(struct v4l2_subdev *subdev, int enable)
{
	int ret;
	u64 streams_mask = BIT(0);

	if (enable)
		ret = v4l2_subdev_enable_streams(subdev, MAX96714_PAD_SOURCE,
						 streams_mask);
	else
		ret = v4l2_subdev_disable_streams(subdev, MAX96714_PAD_SOURCE,
						  streams_mask);
	return ret;
}

static int max96714_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct max96714_priv *priv = sd_to_max96714(sd);
	struct v4l2_mbus_framefmt *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    priv->enabled_source_streams)
		return -EBUSY;

	/* No transcoding, source and sink formats must match. */
	if (format->pad == MAX96714_PAD_SOURCE)
		return v4l2_subdev_get_fmt(sd, state, format);

	fmt = v4l2_subdev_state_get_format(state, format->pad, format->stream);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	fmt = v4l2_subdev_state_get_opposite_stream_format(state, format->pad,
							   format->stream);
	if (!fmt)
		return -EINVAL;

	*fmt = format->format;

	return 0;
}

static int _max96714_set_routing(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 enum v4l2_subdev_format_whence which,
				 struct v4l2_subdev_krouting *routing)
{
	int ret;

	static const struct v4l2_mbus_framefmt format = {
		.width = 1280,
		.height = 1080,
		.code = MEDIA_BUS_FMT_Y8_1X8,
		.field = V4L2_FIELD_NONE,
	};

	/*
	 * Note: we can only support up to V4L2_FRAME_DESC_ENTRY_MAX, until
	 * frame desc is made dynamically allocated.
	 */
	if (routing->num_routes > V4L2_FRAME_DESC_ENTRY_MAX)
		return -EINVAL;

	ret = v4l2_subdev_routing_validate(sd, routing,
					   V4L2_SUBDEV_ROUTING_ONLY_1_TO_1);
	if (ret)
		return ret;

	ret = v4l2_subdev_set_routing_with_fmt(sd, state, routing, &format);
	if (ret)
		return ret;

	return 0;
}

static int max96714_set_routing(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				enum v4l2_subdev_format_whence which,
				struct v4l2_subdev_krouting *routing)
{
	struct max96714_priv *priv = sd_to_max96714(sd);


	if (which == V4L2_SUBDEV_FORMAT_ACTIVE && priv->enabled_source_streams)
		return -EBUSY;

	return _max96714_set_routing(sd, state, which, routing);
}

static int max96714_init_cfg(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_route routes[] = {
		{
			.sink_pad = MAX96714_PAD_SINK,
			.sink_stream = 0,
			.source_pad = MAX96714_PAD_SOURCE,
			.source_stream = 0,
			.flags = V4L2_SUBDEV_ROUTE_FL_ACTIVE,
		}
	};

	struct v4l2_subdev_krouting routing = {
		.num_routes = ARRAY_SIZE(routes),
		.routes = routes,
	};

	return _max96714_set_routing(sd, state, V4L2_SUBDEV_FORMAT_ACTIVE,
				     &routing);
}

static const struct v4l2_subdev_pad_ops max96714_pad_ops = {
	.enable_streams = max96714_enable_streams,
	.disable_streams = max96714_disable_streams,

	.set_routing = max96714_set_routing,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = max96714_set_fmt,

	.init_cfg = max96714_init_cfg,
};

static bool max96714_link_locked(struct max96714_priv *priv)
{
	u8 val;

	max96714_read(priv, MAX96714_LINK_LOCK, &val);

	return val & MAX96714_LINK_LOCK_BIT;
}

static void max96714_link_status(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;

	dev_info(dev, "Link locked:%d\n", max96714_link_locked(priv));
}

static bool max96714_pipe_locked(struct max96714_priv *priv)
{
	u8 val;

	max96714_read(priv, MAX96714_VIDEO_RX8, &val);

	return val & MAX96714_VID_LOCK;
}

static void max96714_pipe_status(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;

	dev_info(dev, "Pipe vidlock:%d\n", max96714_pipe_locked(priv));
}

static void max96714_csi_status(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;
	u8 freq;

	max96714_read(priv, MAX96714_BACKTOP25, &freq);
	freq = FIELD_GET(CSI_DPLL_FREQ_MASK, freq);

	dev_info(dev, "CSI controller DPLL freq:%u00MHz CSIPHY enabled:%d\n",
		 freq, max96714_tx_port_enabled(priv));
}

static int max96714_log_status(struct v4l2_subdev *sd)
{
	struct max96714_priv *priv = sd_to_max96714(sd);
	struct device *dev = &priv->client->dev;

	dev_info(dev, "Deserializer: max96714\n");

	max96714_link_status(priv);
	max96714_pipe_status(priv);
	max96714_csi_status(priv);

	return 0;
}

static const struct v4l2_subdev_core_ops max96714_subdev_core_ops = {
	.log_status = max96714_log_status,
};

static const struct v4l2_subdev_video_ops max96714_video_ops = {
	.s_stream	= max96714_s_stream,
};

static const struct v4l2_subdev_ops max96714_subdev_ops = {
	.video = &max96714_video_ops,
	.core = &max96714_subdev_core_ops,
	.pad = &max96714_pad_ops,
};

static const struct media_entity_operations max96714_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max96714_notify_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_connection *asd)
{
	struct max96714_priv *priv = sd_to_max96714(notifier->sd);
	struct device *dev = &priv->client->dev;
	int ret;

	ret = media_entity_get_fwnode_pad(&subdev->entity,
					  priv->rxport.source.ep_fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (ret < 0) {
		dev_err(dev, "Failed to find pad for %s\n", subdev->name);
		return ret;
	}

	priv->rxport.source.sd = subdev;
	priv->rxport.source.pad = ret;

	ret = media_create_pad_link(&priv->rxport.source.sd->entity,
				    priv->rxport.source.pad, &priv->sd.entity,
				    MAX96714_PAD_SINK,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(dev, "Unable to link %s:%u -> %s:%u\n",
			priv->rxport.source.sd->name, priv->rxport.source.pad,
			priv->sd.name, MAX96714_PAD_SINK);
		return ret;
	}

	return 0;
}

static const struct v4l2_async_notifier_operations max96714_notify_ops = {
	.bound = max96714_notify_bound,
};

static int max96714_v4l2_notifier_register(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct max96714_rxport *rxport = &priv->rxport;
	struct v4l2_async_connection *asd;
	int ret;

	v4l2_async_subdev_nf_init(&priv->notifier, &priv->sd);

	asd = v4l2_async_nf_add_fwnode(&priv->notifier,
				       rxport->source.ep_fwnode,
				       struct v4l2_async_connection);
	if (IS_ERR(asd)) {
		dev_err(dev, "Failed to add subdev: %pe", asd);
		v4l2_async_nf_cleanup(&priv->notifier);
		return PTR_ERR(asd);
	}

	priv->notifier.ops = &max96714_notify_ops;

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret) {
		dev_err(dev, "Failed to register subdev_notifier");
		v4l2_async_nf_cleanup(&priv->notifier);
		return ret;
	}

	return 0;
}

static void max96714_v4l2_notifier_unregister(struct max96714_priv *priv)
{
	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
}

static int max96714_create_subdev(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;
	int ret;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &max96714_subdev_ops);

	v4l2_ctrl_handler_init(&priv->ctrl_handler, 1);
	priv->sd.ctrl_handler = &priv->ctrl_handler;

	priv->tx_link_freq = priv->vep.link_frequencies[0];
	if (priv->tx_link_freq < 0)
		return priv->tx_link_freq;

	v4l2_ctrl_new_int_menu(&priv->ctrl_handler, NULL, V4L2_CID_LINK_FREQ,
			       0, 0, &priv->tx_link_freq);
	if (priv->ctrl_handler.error) {
		ret = priv->ctrl_handler.error;
		goto err_free_ctrl;
	}

	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_STREAMS;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &max96714_entity_ops;

	priv->pads[MAX96714_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	priv->pads[MAX96714_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity,
				     MAX96714_NPORTS,
				     priv->pads);
	if (ret)
		goto err_free_ctrl;

	priv->sd.state_lock = priv->sd.ctrl_handler->lock;

	ret = v4l2_subdev_init_finalize(&priv->sd);
	if (ret)
		goto err_entity_cleanup;

	ret = max96714_v4l2_notifier_register(priv);
	if (ret) {
		dev_err(dev, "v4l2 subdev notifier register failed: %d\n", ret);
		goto err_subdev_cleanup;
	}

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret) {
		dev_err(dev, "v4l2_async_register_subdev error: %d\n", ret);
		goto err_unreg_notif;
	}

	return 0;

err_unreg_notif:
	max96714_v4l2_notifier_unregister(priv);
err_subdev_cleanup:
	v4l2_subdev_cleanup(&priv->sd);
err_entity_cleanup:
	media_entity_cleanup(&priv->sd.entity);
err_free_ctrl:
	v4l2_ctrl_handler_free(&priv->ctrl_handler);

	return ret;

};

static void max96714_destroy_subdev(struct max96714_priv *priv)
{
	max96714_v4l2_notifier_unregister(priv);
	v4l2_async_unregister_subdev(&priv->sd);

	v4l2_subdev_cleanup(&priv->sd);

	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrl_handler);
}

static int max96714_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int max96714_i2c_mux_init(struct max96714_priv *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev,
				  1, 0, I2C_MUX_LOCKED | I2C_MUX_GATE,
				  max96714_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0, 0);
}

static void max96714_init_tx_port(struct max96714_priv *priv)
{
	struct v4l2_mbus_config_mipi_csi2 *mipi;
	unsigned long lanes_used = 0;
	u8 val, lane;

	max96714_disable_tx_port(priv);

	mipi = &priv->vep.bus.mipi_csi2;
	val = *priv->vep.link_frequencies * 2 / MHZ(100);

	max96714_update_bits(priv, MAX96714_BACKTOP25,
			     CSI_DPLL_FREQ_MASK, val);

	val = FIELD_PREP(MAX96714_CSI2_LANE_CNT_MASK, mipi->num_data_lanes - 1);
	max96714_update_bits(priv, MAX96714_MIPI_LANE_CNT,
			     MAX96714_CSI2_LANE_CNT_MASK, val);

	/* lanes polarity */
	val = 0;
	for (lane = 0; lane < mipi->num_data_lanes + 1; lane++) {
		if (!mipi->lane_polarities[lane])
			continue;
		if (lane == 0)
			/* clock lane */
			val |= BIT(5);
		else if (lane < 3)
			/* Lane D0 and D1 */
			val |= BIT(lane - 1);
		else
			/* D2 and D3 */
			val |= BIT(lane);
	}

	max96714_update_bits(priv, MAX96714_MIPI_POLARITY,
			     MAX96714_MIPI_POLARITY_MASK, val);

	/* lanes mapping */
	val = 0;
	for (lane = 0; lane < mipi->num_data_lanes; lane++) {
		val |= (mipi->data_lanes[lane] - 1) << (lane*2);
		lanes_used |= BIT(mipi->data_lanes[lane] - 1);
	}

	/* Unused lanes need to be mapped as well to not have
	 * the same lanes mapped twice.
	 */
	for (; lane < 4; lane++) {
		unsigned int idx = find_first_zero_bit(&lanes_used, 4);

		val |= idx << (lane * 2);
		lanes_used |= BIT(idx);
	}

	max96714_write(priv, MAX96714_MIPI_LANE_MAP, val);
}

static int max96714_rxport_enable_poc(struct max96714_priv *priv)
{
	struct max96714_rxport *rxport = &priv->rxport;

	if (!rxport->poc)
		return 0;

	return regulator_enable(rxport->poc);
}

static void max96714_rxport_disable_poc(struct max96714_priv *priv)
{
	struct max96714_rxport *rxport = &priv->rxport;

	if (rxport->poc)
		regulator_disable(rxport->poc);
}

static int max96714_parse_dt_txport(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;
	struct fwnode_handle *ep_fwnode;
	u32 num_data_lanes;
	s64 dpll_freq;
	int ret;

	ep_fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
						    MAX96714_PAD_SOURCE, 0, 0);
	if (!ep_fwnode)
		return -EINVAL;

	priv->vep.bus_type = V4L2_MBUS_UNKNOWN;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep_fwnode, &priv->vep);
	fwnode_handle_put(ep_fwnode);
	if (ret) {
		dev_err(dev, "tx: failed to parse endpoint data\n");
		return -EINVAL;
	}

	if (priv->vep.bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(&priv->client->dev, "Unsupported bus-type %u\n",
			priv->vep.bus_type);
		return -EINVAL;
	}

	if (priv->vep.nr_of_link_frequencies != 1) {
		ret = -EINVAL;
		goto err_free_vep;
	}

	/* DPLL freq is twice the link frequency */
	dpll_freq = priv->vep.link_frequencies[0] * 2;

	/* 100Mbps step, Min 100Mbps, Max 2500Mbps */
	if (dpll_freq % MHZ(100) || dpll_freq < MHZ(100) ||
	    dpll_freq > MHZ(2500)) {
		dev_err(dev, "tx: invalid link frequency\n");
		ret = -EINVAL;
		goto err_free_vep;
	}

	num_data_lanes = priv->vep.bus.mipi_csi2.num_data_lanes;
	if (num_data_lanes < 1 || num_data_lanes > 4) {
		dev_err(dev,
			"tx: invalid number of data lanes should be 1 to 4\n");
		ret = -EINVAL;
		goto err_free_vep;
	}

	return 0;

err_free_vep:
	v4l2_fwnode_endpoint_free(&priv->vep);

	return ret;
};

static int max96714_parse_dt_rxport(struct max96714_priv *priv)
{
	static const char *poc_name = "port0-poc";
	struct max96714_rxport *rxport = &priv->rxport;
	struct device *dev = &priv->client->dev;
	struct fwnode_handle *ep_fwnode;
	int ret;

	ep_fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev),
						    MAX96714_PAD_SINK,
						    0, 0);
	if (!ep_fwnode)
		return -ENOENT;

	rxport->source.ep_fwnode = fwnode_graph_get_remote_endpoint(ep_fwnode);
	fwnode_handle_put(ep_fwnode);

	if (!rxport->source.ep_fwnode) {
		dev_err(dev, "rx: no remote endpoint\n");
		return -EINVAL;
	}

	rxport->poc = devm_regulator_get_optional(dev, poc_name);
	if (IS_ERR(rxport->poc)) {
		ret = PTR_ERR(rxport->poc);
		if (ret == -ENODEV) {
			rxport->poc = NULL;
		} else {
			dev_err(dev, "rx: failed to get POC supply: %d\n", ret);
			goto err_put_source_ep_fwnode;
		}
	}

	return 0;

err_put_source_ep_fwnode:
	fwnode_handle_put(rxport->source.ep_fwnode);
	return ret;
}

static int max96714_parse_dt(struct max96714_priv *priv)
{
	int ret;

	ret = max96714_parse_dt_rxport(priv);
	if (ret)
		return ret;

	ret = max96714_parse_dt_txport(priv);
	if (ret)
		goto err_put_fwnode;

	return 0;

err_put_fwnode:
	fwnode_handle_put(priv->rxport.source.ep_fwnode);

	return ret;
}

static int max96714_enable_core_hw(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;
	u8 val;
	int ret;

	if (priv->gpiod_pwdn) {
		/* wait min 2 ms for reset to complete */
		gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);
		fsleep(2000);
		gpiod_set_value_cansleep(priv->gpiod_pwdn, 1);
		/* wait min 2 ms for power up to finish */
		fsleep(2000);
	}

	ret = max96714_read(priv, MAX96714_REG13, &val);
	if (ret) {
		dev_err_probe(dev, ret, "Cannot read first register, abort\n");
		goto err_pd_gpio;
	}

	if (val != MAX96714F_DEVICE_ID) {
		dev_err(dev, "Unsupported device id expected %x got %x\n",
			MAX96714F_DEVICE_ID, val);
		ret = -EOPNOTSUPP;
		goto err_pd_gpio;
	}

	ret = max96714_read(priv, MAX96714_DEV_REV, &val);
	if (ret)
		goto err_pd_gpio;

	dev_dbg(dev, "Found %x (rev %lx)\n", MAX96714F_DEVICE_ID,
		val & MAX96714_DEV_REV_MASK);

	ret = max96714_read(priv, MAX96714_MIPI_TX52, &val);
	if (ret)
		goto err_pd_gpio;

	if (!(val & MAX96714_TUN_EN)) {
		dev_err(dev, "Only supporting tunnel mode");
		ret = -EOPNOTSUPP;
		goto err_pd_gpio;
	}

	return 0;

err_pd_gpio:
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);
	return ret;
}

static void max96714_disable_core_hw(struct max96714_priv *priv)
{
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);
}

static int max96714_get_hw_resources(struct max96714_priv *priv)
{
	struct device *dev = &priv->client->dev;

	priv->regmap = devm_regmap_init_i2c(priv->client,
					    &max96714_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->gpiod_pwdn = devm_gpiod_get_optional(&priv->client->dev, "enable",
						   GPIOD_OUT_HIGH);
	if (IS_ERR(priv->gpiod_pwdn))
		return dev_err_probe(dev, PTR_ERR(priv->gpiod_pwdn),
				     "Cannot get enable GPIO\n");
	gpiod_set_consumer_name(priv->gpiod_pwdn, "maxgmsl2-pwdn");

	return 0;
}

static int max96714_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max96714_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->regmap = devm_regmap_init_i2c(priv->client,
					    &max96714_regmap_config);

	ret = max96714_get_hw_resources(priv);
	if (ret)
		return ret;

	ret = max96714_enable_core_hw(priv);
	if (ret)
		return ret;

	ret = max96714_parse_dt(priv);
	if (ret)
		goto err_disable_core_hw;

	max96714_init_tx_port(priv);

	ret = max96714_rxport_enable_poc(priv);
	if (ret)
		goto err_free_ports;

	ret = max96714_i2c_mux_init(priv);
	if (ret)
		goto err_disable_poc;

	ret = max96714_create_subdev(priv);
	if (ret)
		goto err_del_mux;

	return 0;

err_del_mux:
	i2c_mux_del_adapters(priv->mux);
err_disable_poc:
	max96714_rxport_disable_poc(priv);
err_free_ports:
	fwnode_handle_put(priv->rxport.source.ep_fwnode);
	v4l2_fwnode_endpoint_free(&priv->vep);
err_disable_core_hw:
	max96714_disable_core_hw(priv);

	return ret;
}

static void max96714_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96714_priv *priv = sd_to_max96714(sd);

	max96714_destroy_subdev(priv);
	i2c_mux_del_adapters(priv->mux);
	max96714_rxport_disable_poc(priv);
	fwnode_handle_put(priv->rxport.source.ep_fwnode);
	v4l2_fwnode_endpoint_free(&priv->vep);
	max96714_disable_core_hw(priv);
	gpiod_set_value_cansleep(priv->gpiod_pwdn, 0);
}

static const struct of_device_id max96714_of_ids[] = {
	{ .compatible = "maxim,max96714f" },
	{ }
};
MODULE_DEVICE_TABLE(of, max96714_of_ids);

static struct i2c_driver max96714_i2c_driver = {
	.driver	= {
		.name		= "max96714",
		.of_match_table	= max96714_of_ids,
	},
	.probe		= max96714_probe,
	.remove		= max96714_remove,
};

module_i2c_driver(max96714_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Maxim Integrated GMSL2 Deserializers Driver");
MODULE_AUTHOR("Julien Massot <julien.massot@collabora.com>");
