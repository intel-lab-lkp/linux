// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Toradex
 * Author: Francesco Dolcini <francesco.dolcini@toradex.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>

static const struct regulator_bulk_data thc63lvd827_supplies[] = {
	{ .supply = "vcc" },
	{ .supply = "pllvcc" },
	{ .supply = "lvdsvcc" },
	{ .supply = "iovcc" },
};

struct thc63lvd827 {
	struct device *dev;
	struct drm_bridge bridge;
	struct regulator_bulk_data *supplies;

	struct gpio_desc *map_gpio;
	struct gpio_desc *mode_gpio;
	struct gpio_desc *b6b8_gpio;
	struct gpio_desc *pd_gpio;

	struct drm_bridge_timings timings;
};

static struct thc63lvd827 *bridge_to_thc63lvd827(struct drm_bridge *bridge)
{
	return container_of(bridge, struct thc63lvd827, bridge);
}

static int thc63lvd827_attach(struct drm_bridge *bridge,
			      struct drm_encoder *encoder,
			      enum drm_bridge_attach_flags flags)
{
	return drm_bridge_attach(encoder, bridge->next_bridge, bridge, flags);
}

static u32 *
thc63lvd827_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
				      struct drm_bridge_state *bridge_state,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state,
				      u32 output_fmt,
				      unsigned int *num_input_fmts)
{
	u32 *input_fmts;

	*num_input_fmts = 0;

	input_fmts = kmalloc_obj(*input_fmts);
	if (!input_fmts)
		return NULL;

	input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_input_fmts = 1;

	return input_fmts;
}

static int thc63lvd827_atomic_check(struct drm_bridge *bridge,
				    struct drm_bridge_state *bridge_state,
				    struct drm_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state)
{
	struct thc63lvd827 *thc63lvd827 = bridge_to_thc63lvd827(bridge);

	bridge_state->input_bus_cfg.flags = thc63lvd827->timings.input_bus_flags;

	return 0;
}

static void thc63lvd827_atomic_pre_enable(struct drm_bridge *bridge,
					  struct drm_atomic_commit *state)
{
	struct thc63lvd827 *thc63lvd827 = bridge_to_thc63lvd827(bridge);
	const struct drm_bridge_state *bridge_state;
	int b6b8_val, map_val, ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(thc63lvd827_supplies),
				    thc63lvd827->supplies);
	if (ret) {
		dev_err(thc63lvd827->dev, "failed to enable regulators: %d\n", ret);
		return;
	}

	bridge_state = drm_atomic_get_new_bridge_state(state, bridge);

	/*
	 * 6B/8B: H = 6-bit (21-bit) mode, L = 8-bit (27-bit) mode.
	 * MAP: H = Mapping Mode 1, L = Mapping Mode 2.
	 */
	switch (bridge_state->output_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		b6b8_val = 1;
		map_val = 1;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		b6b8_val = 0;
		map_val = 1;
		break;
	default:
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		b6b8_val = 0;
		map_val = 0;
		break;
	}

	gpiod_set_value_cansleep(thc63lvd827->b6b8_gpio, b6b8_val);
	gpiod_set_value_cansleep(thc63lvd827->map_gpio, map_val);
	gpiod_set_value_cansleep(thc63lvd827->pd_gpio, 0);
}

static void thc63lvd827_atomic_post_disable(struct drm_bridge *bridge,
					    struct drm_atomic_commit *state)
{
	struct thc63lvd827 *thc63lvd827 = bridge_to_thc63lvd827(bridge);
	int ret;

	gpiod_set_value_cansleep(thc63lvd827->pd_gpio, 1);

	ret = regulator_bulk_disable(ARRAY_SIZE(thc63lvd827_supplies),
				     thc63lvd827->supplies);
	if (ret)
		dev_err(thc63lvd827->dev, "failed to disable regulators: %d\n", ret);
}

static const struct drm_bridge_funcs thc63lvd827_bridge_funcs = {
	.attach = thc63lvd827_attach,
	.atomic_check = thc63lvd827_atomic_check,
	.atomic_pre_enable = thc63lvd827_atomic_pre_enable,
	.atomic_post_disable = thc63lvd827_atomic_post_disable,
	.atomic_get_input_bus_fmts = thc63lvd827_atomic_get_input_bus_fmts,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
};

static int thc63lvd827_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thc63lvd827 *thc63lvd827;
	struct device_node *ep;
	u32 pclk_sample = 0;
	int ret;

	thc63lvd827 = devm_drm_bridge_alloc(dev, struct thc63lvd827, bridge,
					    &thc63lvd827_bridge_funcs);
	if (IS_ERR(thc63lvd827))
		return PTR_ERR(thc63lvd827);

	thc63lvd827->dev = dev;

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(thc63lvd827_supplies),
					    thc63lvd827_supplies, &thc63lvd827->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	thc63lvd827->map_gpio = devm_gpiod_get_optional(dev, "map", GPIOD_OUT_LOW);
	if (IS_ERR(thc63lvd827->map_gpio))
		return dev_err_probe(dev, PTR_ERR(thc63lvd827->map_gpio),
				     "failed to get MAP GPIO\n");

	thc63lvd827->mode_gpio = devm_gpiod_get_optional(dev, "mode", GPIOD_OUT_HIGH);
	if (IS_ERR(thc63lvd827->mode_gpio))
		return dev_err_probe(dev, PTR_ERR(thc63lvd827->mode_gpio),
				     "failed to get MODE GPIO\n");

	thc63lvd827->b6b8_gpio = devm_gpiod_get_optional(dev, "b6b8", GPIOD_OUT_LOW);
	if (IS_ERR(thc63lvd827->b6b8_gpio))
		return dev_err_probe(dev, PTR_ERR(thc63lvd827->b6b8_gpio),
				     "failed to get 6B/8B GPIO\n");

	thc63lvd827->pd_gpio = devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(thc63lvd827->pd_gpio))
		return dev_err_probe(dev, PTR_ERR(thc63lvd827->pd_gpio),
				     "failed to get /PDWN GPIO\n");

	thc63lvd827->bridge.next_bridge = of_drm_get_bridge_by_endpoint(dev->of_node, 1, 0);
	if (IS_ERR(thc63lvd827->bridge.next_bridge))
		return dev_err_probe(dev, PTR_ERR(thc63lvd827->bridge.next_bridge),
				     "failed to get downstream bridge\n");

	ep = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	of_property_read_u32(ep, "pclk-sample", &pclk_sample);
	of_node_put(ep);

	if (pclk_sample == 1)
		thc63lvd827->timings.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_POSEDGE;
	else
		thc63lvd827->timings.input_bus_flags = DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE;

	thc63lvd827->bridge.of_node = dev->of_node;
	thc63lvd827->bridge.timings = &thc63lvd827->timings;
	thc63lvd827->bridge.type = DRM_MODE_CONNECTOR_LVDS;

	return devm_drm_bridge_add(dev, &thc63lvd827->bridge);
}

static const struct of_device_id thc63lvd827_of_match[] = {
	{ .compatible = "thine,thc63lvd827" },
	{ }
};
MODULE_DEVICE_TABLE(of, thc63lvd827_of_match);

static struct platform_driver thc63lvd827_driver = {
	.probe = thc63lvd827_probe,
	.driver = {
		.name = "thc63lvd827",
		.of_match_table = thc63lvd827_of_match,
	},
};
module_platform_driver(thc63lvd827_driver);

MODULE_DESCRIPTION("THine THC63LVD827 DPI to LVDS bridge driver");
MODULE_LICENSE("GPL");
