// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015-2016 Free Electrons
 * Copyright (C) 2015-2016 NextThing Co
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

struct simple_bridge_info {
	const struct drm_bridge_timings *timings;
	unsigned int connector_type;
	const struct drm_bridge_funcs *bridge_funcs;
};

struct simple_bridge {
	struct drm_bridge	bridge;
	struct drm_connector	connector;

	const struct simple_bridge_info *info;

	struct drm_bridge	*next_bridge;
	struct drm_panel	*next_panel;
	struct regulator	*vdd;
	struct gpio_desc	*enable;

	int			dpi_color_coding_input;
	int			dpi_color_coding_output;
};

static inline struct simple_bridge *
drm_bridge_to_simple_bridge(struct drm_bridge *bridge)
{
	return container_of(bridge, struct simple_bridge, bridge);
}

static inline struct simple_bridge *
drm_connector_to_simple_bridge(struct drm_connector *connector)
{
	return container_of(connector, struct simple_bridge, connector);
}

static int simple_bridge_get_modes(struct drm_connector *connector)
{
	struct simple_bridge *sbridge = drm_connector_to_simple_bridge(connector);
	const struct drm_edid *drm_edid;
	int ret;

	if (sbridge->next_bridge->ops & DRM_BRIDGE_OP_EDID) {
		drm_edid = drm_bridge_edid_read(sbridge->next_bridge, connector);
		if (!drm_edid)
			DRM_INFO("EDID read failed. Fallback to standard modes\n");
	} else {
		drm_edid = NULL;
	}

	drm_edid_connector_update(connector, drm_edid);

	if (!drm_edid) {
		/*
		 * In case we cannot retrieve the EDIDs (missing or broken DDC
		 * bus from the next bridge), fallback on the XGA standards and
		 * prefer a mode pretty much anyone can handle.
		 */
		ret = drm_add_modes_noedid(connector, 1920, 1200);
		drm_set_preferred_mode(connector, 1024, 768);
		return ret;
	}

	ret = drm_edid_connector_add_modes(connector);
	drm_edid_free(drm_edid);

	return ret;
}

static const struct drm_connector_helper_funcs simple_bridge_con_helper_funcs = {
	.get_modes	= simple_bridge_get_modes,
};

static enum drm_connector_status
simple_bridge_connector_detect(struct drm_connector *connector, bool force)
{
	struct simple_bridge *sbridge = drm_connector_to_simple_bridge(connector);

	return drm_bridge_detect(sbridge->next_bridge);
}

static const struct drm_connector_funcs simple_bridge_con_funcs = {
	.detect			= simple_bridge_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int simple_bridge_attach(struct drm_bridge *bridge,
				enum drm_bridge_attach_flags flags)
{
	struct simple_bridge *sbridge = drm_bridge_to_simple_bridge(bridge);
	int ret;

	if (sbridge->next_panel)
		return drm_bridge_attach(bridge->encoder, sbridge->next_bridge,
					 bridge, flags);

	ret = drm_bridge_attach(bridge->encoder, sbridge->next_bridge, bridge,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret < 0)
		return ret;

	if (flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)
		return 0;

	drm_connector_helper_add(&sbridge->connector,
				 &simple_bridge_con_helper_funcs);
	ret = drm_connector_init_with_ddc(bridge->dev, &sbridge->connector,
					  &simple_bridge_con_funcs,
					  sbridge->info->connector_type,
					  sbridge->next_bridge->ddc);
	if (ret) {
		DRM_ERROR("Failed to initialize connector\n");
		return ret;
	}

	drm_connector_attach_encoder(&sbridge->connector, bridge->encoder);

	return 0;
}

static void simple_bridge_enable(struct drm_bridge *bridge)
{
	struct simple_bridge *sbridge = drm_bridge_to_simple_bridge(bridge);
	int ret;

	if (sbridge->vdd) {
		ret = regulator_enable(sbridge->vdd);
		if (ret)
			DRM_ERROR("Failed to enable vdd regulator: %d\n", ret);
	}

	gpiod_set_value_cansleep(sbridge->enable, 1);
}

static void simple_bridge_disable(struct drm_bridge *bridge)
{
	struct simple_bridge *sbridge = drm_bridge_to_simple_bridge(bridge);

	gpiod_set_value_cansleep(sbridge->enable, 0);

	if (sbridge->vdd)
		regulator_disable(sbridge->vdd);
}

static const struct drm_bridge_funcs default_simple_bridge_bridge_funcs = {
	.attach		= simple_bridge_attach,
	.enable		= simple_bridge_enable,
	.disable	= simple_bridge_disable,
};

static u32 *
dpi_color_encoder_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
					    struct drm_bridge_state *bridge_state,
					    struct drm_crtc_state *crtc_state,
					    struct drm_connector_state *conn_state,
					    u32 output_fmt,
					    unsigned int *num_input_fmts)
{
	struct simple_bridge *sbridge = drm_bridge_to_simple_bridge(bridge);
	u32 *input_fmts;

	*num_input_fmts = 0;

	if (sbridge->dpi_color_coding_output != output_fmt)
		return NULL;

	input_fmts = kzalloc(sizeof(*input_fmts), GFP_KERNEL);
	if (!input_fmts)
		return NULL;

	*num_input_fmts = 1;
	input_fmts[0] = sbridge->dpi_color_coding_input;
	return input_fmts;
}

static const struct drm_bridge_funcs dpi_color_encoder_bridge_funcs = {
	.attach				= simple_bridge_attach,
	.enable				= simple_bridge_enable,
	.disable			= simple_bridge_disable,
	.atomic_reset			= drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state		= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state		= drm_atomic_helper_bridge_destroy_state,
	.atomic_get_input_bus_fmts	= dpi_color_encoder_atomic_get_input_bus_fmts,
};

static int simple_bridge_get_dpi_color_coding(struct simple_bridge *sbridge,
					      struct device *dev)
{
	struct device_node *ep0, *ep1 = NULL;
	int ret = 0;

	ep0 = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	if (!ep0) {
		dev_err(dev, "failed to get port@0 endpoint\n");
		ret = -ENODEV;
		goto out;
	}

	ep1 = of_graph_get_endpoint_by_regs(dev->of_node, 1, 0);
	if (!ep1) {
		dev_err(dev, "failed to get port@1 endpoint\n");
		ret = -ENODEV;
		goto out;
	}

	sbridge->dpi_color_coding_input = drm_of_dpi_get_color_coding(ep0);
	if (sbridge->dpi_color_coding_input < 0) {
		dev_err(dev, "failed to get DPI input media bus format\n");
		ret = sbridge->dpi_color_coding_input;
		goto out;
	}

	sbridge->dpi_color_coding_output = drm_of_dpi_get_color_coding(ep1);
	if (sbridge->dpi_color_coding_output < 0) {
		dev_err(dev, "failed to get DPI output media bus format\n");
		ret = sbridge->dpi_color_coding_output;
		goto out;
	}

out:
	of_node_put(ep1);
	of_node_put(ep0);

	return ret;
}

static int simple_bridge_probe(struct platform_device *pdev)
{
	struct simple_bridge *sbridge;
	int ret;

	sbridge = devm_kzalloc(&pdev->dev, sizeof(*sbridge), GFP_KERNEL);
	if (!sbridge)
		return -ENOMEM;

	sbridge->info = of_device_get_match_data(&pdev->dev);

	/* Get the next bridge in the pipeline. */
	ret = drm_of_find_panel_or_bridge(pdev->dev.of_node, 1, -1,
					  &sbridge->next_panel,
					  &sbridge->next_bridge);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Next panel or bridge not found\n");

	if (sbridge->next_panel)
		sbridge->next_bridge = devm_drm_panel_bridge_add(&pdev->dev,
								 sbridge->next_panel);

	if (IS_ERR(sbridge->next_bridge))
		return dev_err_probe(&pdev->dev, PTR_ERR(sbridge->next_bridge),
				     "Next bridge not found\n");

	/* Get the regulator and GPIO resources. */
	sbridge->vdd = devm_regulator_get_optional(&pdev->dev, "vdd");
	if (IS_ERR(sbridge->vdd)) {
		int ret = PTR_ERR(sbridge->vdd);
		if (ret == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		sbridge->vdd = NULL;
		dev_dbg(&pdev->dev, "No vdd regulator found: %d\n", ret);
	}

	sbridge->enable = devm_gpiod_get_optional(&pdev->dev, "enable",
						  GPIOD_OUT_LOW);
	if (IS_ERR(sbridge->enable))
		return dev_err_probe(&pdev->dev, PTR_ERR(sbridge->enable),
				     "Unable to retrieve enable GPIO\n");

	if (of_device_is_compatible(pdev->dev.of_node, "dpi-color-encoder")) {
		ret = simple_bridge_get_dpi_color_coding(sbridge, &pdev->dev);
		if (ret)
			return ret;
	}

	/* Register the bridge. */
	sbridge->bridge.funcs = sbridge->info->bridge_funcs;
	sbridge->bridge.of_node = pdev->dev.of_node;
	sbridge->bridge.timings = sbridge->info->timings;

	return devm_drm_bridge_add(&pdev->dev, &sbridge->bridge);
}

/*
 * We assume the ADV7123 DAC is the "default" for historical reasons
 * Information taken from the ADV7123 datasheet, revision D.
 * NOTE: the ADV7123EP seems to have other timings and need a new timings
 * set if used.
 */
static const struct drm_bridge_timings default_bridge_timings = {
	/* Timing specifications, datasheet page 7 */
	.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	.setup_time_ps = 500,
	.hold_time_ps = 1500,
};

/*
 * Information taken from the THS8134, THS8134A, THS8134B datasheet named
 * "SLVS205D", dated May 1990, revised March 2000.
 */
static const struct drm_bridge_timings ti_ths8134_bridge_timings = {
	/* From timing diagram, datasheet page 9 */
	.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	/* From datasheet, page 12 */
	.setup_time_ps = 3000,
	/* I guess this means latched input */
	.hold_time_ps = 0,
};

/*
 * Information taken from the THS8135 datasheet named "SLAS343B", dated
 * May 2001, revised April 2013.
 */
static const struct drm_bridge_timings ti_ths8135_bridge_timings = {
	/* From timing diagram, datasheet page 14 */
	.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE,
	/* From datasheet, page 16 */
	.setup_time_ps = 2000,
	.hold_time_ps = 500,
};

static const struct of_device_id simple_bridge_match[] = {
	{
		.compatible = "dumb-vga-dac",
		.data = &(const struct simple_bridge_info) {
			.connector_type = DRM_MODE_CONNECTOR_VGA,
			.bridge_funcs = &default_simple_bridge_bridge_funcs,
		},
	}, {
		.compatible = "adi,adv7123",
		.data = &(const struct simple_bridge_info) {
			.timings = &default_bridge_timings,
			.connector_type = DRM_MODE_CONNECTOR_VGA,
			.bridge_funcs = &default_simple_bridge_bridge_funcs,
		},
	}, {
		.compatible = "dpi-color-encoder",
		.data = &(const struct simple_bridge_info) {
			.connector_type = DRM_MODE_CONNECTOR_DPI,
			.bridge_funcs = &dpi_color_encoder_bridge_funcs,
		},
	}, {
		.compatible = "ti,opa362",
		.data = &(const struct simple_bridge_info) {
			.connector_type = DRM_MODE_CONNECTOR_Composite,
			.bridge_funcs = &default_simple_bridge_bridge_funcs,
		},
	}, {
		.compatible = "ti,ths8135",
		.data = &(const struct simple_bridge_info) {
			.timings = &ti_ths8135_bridge_timings,
			.connector_type = DRM_MODE_CONNECTOR_VGA,
			.bridge_funcs = &default_simple_bridge_bridge_funcs,
		},
	}, {
		.compatible = "ti,ths8134",
		.data = &(const struct simple_bridge_info) {
			.timings = &ti_ths8134_bridge_timings,
			.connector_type = DRM_MODE_CONNECTOR_VGA,
			.bridge_funcs = &default_simple_bridge_bridge_funcs,
		},
	},
	{},
};
MODULE_DEVICE_TABLE(of, simple_bridge_match);

static struct platform_driver simple_bridge_driver = {
	.probe	= simple_bridge_probe,
	.driver		= {
		.name		= "simple-bridge",
		.of_match_table	= simple_bridge_match,
	},
};
module_platform_driver(simple_bridge_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Simple DRM bridge driver");
MODULE_LICENSE("GPL");
