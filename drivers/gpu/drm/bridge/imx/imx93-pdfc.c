// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright 2022-2024 NXP
 */

#include <linux/media-bus-format.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_print.h>

#define DISPLAY_MUX		0x60
#define  PARALLEL_DISP_FORMAT	0x700

enum imx93_pdfc_format {
	RGB888_TO_RGB888 = 0x0,
	RGB888_TO_RGB666 = 0x1 << 8,
	RGB565_TO_RGB565 = 0x2 << 8,
};

struct imx93_pdfc {
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct device *dev;
	struct regmap *regmap;
	u32 format;
};

static int imx93_pdfc_bridge_attach(struct drm_bridge *bridge,
				    enum drm_bridge_attach_flags flags)
{
	struct imx93_pdfc *pdfc = bridge->driver_private;

	return drm_bridge_attach(bridge->encoder, pdfc->next_bridge, bridge, flags);
}

static void
imx93_pdfc_bridge_atomic_enable(struct drm_bridge *bridge,
				struct drm_bridge_state *old_bridge_state)
{
	struct imx93_pdfc *pdfc = bridge->driver_private;

	regmap_update_bits(pdfc->regmap, DISPLAY_MUX, PARALLEL_DISP_FORMAT,
			   pdfc->format);
}

static const u32 imx93_pdfc_bus_output_fmts[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
	MEDIA_BUS_FMT_RGB666_1X18,
	MEDIA_BUS_FMT_RGB565_1X16,
	MEDIA_BUS_FMT_FIXED
};

static bool imx93_pdfc_bus_output_fmt_supported(u32 fmt)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(imx93_pdfc_bus_output_fmts); i++) {
		if (imx93_pdfc_bus_output_fmts[i] == fmt)
			return true;
	}

	return false;
}

static u32 *
imx93_pdfc_bridge_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
					    struct drm_bridge_state *bridge_state,
					    struct drm_crtc_state *crtc_state,
					    struct drm_connector_state *conn_state,
					    u32 output_fmt,
					    unsigned int *num_input_fmts)
{
	u32 *input_fmts;

	*num_input_fmts = 0;

	if (!imx93_pdfc_bus_output_fmt_supported(output_fmt))
		return NULL;

	input_fmts = kmalloc(sizeof(*input_fmts), GFP_KERNEL);
	if (!input_fmts)
		return NULL;

	switch (output_fmt) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB565_1X16:
		input_fmts[0] = output_fmt;
		break;
	case MEDIA_BUS_FMT_RGB666_1X18:
	case MEDIA_BUS_FMT_FIXED:
		input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
		break;
	}

	*num_input_fmts = 1;

	return input_fmts;
}

static int imx93_pdfc_bridge_atomic_check(struct drm_bridge *bridge,
					  struct drm_bridge_state *bridge_state,
					  struct drm_crtc_state *crtc_state,
					  struct drm_connector_state *conn_state)
{
	struct imx93_pdfc *pdfc = bridge->driver_private;

	switch (bridge_state->output_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
		pdfc->format = RGB888_TO_RGB888;
		break;
	case MEDIA_BUS_FMT_RGB666_1X18:
		pdfc->format = RGB888_TO_RGB666;
		break;
	case MEDIA_BUS_FMT_RGB565_1X16:
		pdfc->format = RGB565_TO_RGB565;
		break;
	default:
		DRM_DEV_DEBUG_DRIVER(pdfc->dev, "Unsupported output bus format: 0x%x\n",
				     bridge_state->output_bus_cfg.format);
		return -EINVAL;
	}

	return 0;
}

static const struct drm_bridge_funcs imx93_pdfc_bridge_funcs = {
	.attach			= imx93_pdfc_bridge_attach,
	.atomic_enable		= imx93_pdfc_bridge_atomic_enable,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
	.atomic_get_input_bus_fmts	= imx93_pdfc_bridge_atomic_get_input_bus_fmts,
	.atomic_check		= imx93_pdfc_bridge_atomic_check,
	.atomic_reset		= drm_atomic_helper_bridge_reset,
};

static int imx93_pdfc_bridge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imx93_pdfc *pdfc;

	pdfc = devm_kzalloc(dev, sizeof(*pdfc), GFP_KERNEL);
	if (!pdfc)
		return -ENOMEM;

	pdfc->regmap = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(pdfc->regmap))
		return dev_err_probe(dev, PTR_ERR(pdfc->regmap),
				     "failed to get regmap\n");

	pdfc->next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);
	if (IS_ERR(pdfc->next_bridge))
		return dev_err_probe(dev, PTR_ERR(pdfc->next_bridge),
				     "failed to get next bridge\n");

	platform_set_drvdata(pdev, pdfc);

	pdfc->dev = dev;
	pdfc->bridge.driver_private = pdfc;
	pdfc->bridge.funcs = &imx93_pdfc_bridge_funcs;
	pdfc->bridge.of_node = dev->of_node;

	drm_bridge_add(&pdfc->bridge);

	return 0;
}

static void imx93_pdfc_bridge_remove(struct platform_device *pdev)
{
	struct imx93_pdfc *pdfc = platform_get_drvdata(pdev);

	drm_bridge_remove(&pdfc->bridge);
}

static const struct of_device_id imx93_pdfc_dt_ids[] = {
	{ .compatible = "nxp,imx93-pdfc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx93_pdfc_dt_ids);

static struct platform_driver imx93_pdfc_bridge_driver = {
	.probe	= imx93_pdfc_bridge_probe,
	.remove_new = imx93_pdfc_bridge_remove,
	.driver	= {
		.of_match_table = imx93_pdfc_dt_ids,
		.name = "imx93_pdfc",
	},
};
module_platform_driver(imx93_pdfc_bridge_driver);

MODULE_DESCRIPTION("NXP i.MX93 parallel display format configuration driver");
MODULE_AUTHOR("Liu Ying <victor.liu@nxp.com>");
MODULE_LICENSE("GPL v2");
