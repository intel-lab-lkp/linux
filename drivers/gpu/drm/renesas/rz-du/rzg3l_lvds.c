// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G3L LVDS Encoder Driver
 *
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/media-bus-format.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "rzg3l_lvds_regs.h"

enum rzg3l_lvds_mode {
	RZG3L_LVDS_MODE_JEIDA = 0,
	RZG3L_LVDS_MODE_JEIDA_MIRROR = 1,
	RZG3L_LVDS_MODE_MODE2 = 2,
	RZG3L_LVDS_MODE_MODE2_MIRROR = 3,
	RZG3L_LVDS_MODE_VESA = 4,
	RZG3L_LVDS_MODE_VESA_MIRROR = 5,
	RZG3L_LVDS_MODE_MODE6 = 6,
	RZG3L_LVDS_MODE_MODE6_MIRROR = 7,
};

struct rzg3l_lvds {
	struct device *dev;
	struct reset_control *prstc;
	struct reset_control *lvd_rstc;
	struct regmap *regmap;
	struct drm_bridge bridge;
};

#define bridge_to_rzg3l_lvds(b) \
	container_of(b, struct rzg3l_lvds, bridge)

/* -----------------------------------------------------------------------------
 * Bridge
 */

static void rzg3l_lvds_atomic_enable(struct drm_bridge *bridge,
				     struct drm_atomic_commit *state)
{
	struct rzg3l_lvds *lvds = bridge_to_rzg3l_lvds(bridge);
	const struct drm_bridge_state *bridge_state;
	u32 fmt;

	/* Get the LVDS format from the bridge state. */
	bridge_state = drm_atomic_get_new_bridge_state(state, bridge);
	if (WARN_ON(!bridge_state))
		return;

	switch (bridge_state->output_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		fmt = RZG3L_LVDS_MODE_JEIDA;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		fmt = RZG3L_LVDS_MODE_VESA;
		break;
	default:
		fmt = RZG3L_LVDS_MODE_VESA;
		dev_warn(lvds->dev, "Unsupported bus fmt 0x%04x\n",
			 bridge_state->output_bus_cfg.format);
		break;
	}

	if (WARN_ON(pm_runtime_get_sync(lvds->dev) < 0))
		return;

	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_EN_BGR, LVDS_0_PHY_CH_EN_BGR);
	fsleep(20);

	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_EN_LDO, LVDS_0_PHY_CH_EN_LDO);
	fsleep(10);

	regmap_write(lvds->regmap, LVDS_CMN, LVDS_CMN_RST_PHY0_SEL);
	regmap_update_bits(lvds->regmap, LVDS_0_CTL_OFFSET,
			   LVDS_0_CTL_FMT_SEL_MSK,
			   FIELD_PREP(LVDS_0_CTL_FMT_SEL_MSK, fmt));
	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_IO_EN_MSK, LVDS_0_PHY_CH_IO_EN);
	regmap_write(lvds->regmap, LVDS_CMN,
		     LVDS_CMN_RST_PHY0_SEL | LVDS_CMN_PHY_RESET);
	fsleep(100);
}

static void rzg3l_lvds_atomic_disable(struct drm_bridge *bridge,
				      struct drm_atomic_commit *state)
{
	struct rzg3l_lvds *lvds = bridge_to_rzg3l_lvds(bridge);

	regmap_update_bits(lvds->regmap, LVDS_CMN, LVDS_CMN_PHY_RESET, 0);
	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_IO_EN_MSK, 0);
	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_EN_LDO, 0);
	regmap_update_bits(lvds->regmap, LVDS_0_PHY_OFFSET,
			   LVDS_0_PHY_CH_EN_BGR, 0);

	pm_runtime_put(lvds->dev);
}

static int rzg3l_lvds_attach(struct drm_bridge *bridge,
			     struct drm_encoder *encoder,
			     enum drm_bridge_attach_flags flags)
{
	struct rzg3l_lvds *lvds = bridge_to_rzg3l_lvds(bridge);

	if (!lvds->bridge.next_bridge)
		return 0;

	return drm_bridge_attach(encoder, lvds->bridge.next_bridge, bridge, flags);
}

static enum drm_mode_status
rzg3l_lvds_bridge_mode_valid(struct drm_bridge *bridge,
			     const struct drm_display_info *info,
			     const struct drm_display_mode *mode)
{
	if (mode->clock > 87000)
		return MODE_CLOCK_HIGH;

	if (mode->clock < 25000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

static const struct drm_bridge_funcs rzg3l_lvds_bridge_ops = {
	.attach = rzg3l_lvds_attach,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_enable = rzg3l_lvds_atomic_enable,
	.atomic_disable = rzg3l_lvds_atomic_disable,
	.mode_valid = rzg3l_lvds_bridge_mode_valid,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int rzg3l_lvds_pm_runtime_suspend(struct device *dev)
{
	struct rzg3l_lvds *lvds = dev_get_drvdata(dev);
	struct reset_control_bulk_data resets[] = {
		{ .rstc = lvds->lvd_rstc },
		{ .rstc = lvds->prstc },
	};

	return reset_control_bulk_assert(ARRAY_SIZE(resets), resets);
}

static int rzg3l_lvds_pm_runtime_resume(struct device *dev)
{
	struct rzg3l_lvds *lvds = dev_get_drvdata(dev);
	struct reset_control_bulk_data resets[] = {
		{ .rstc = lvds->lvd_rstc },
		{ .rstc = lvds->prstc },
	};

	return reset_control_bulk_deassert(ARRAY_SIZE(resets), resets);
}

static DEFINE_RUNTIME_DEV_PM_OPS(rzg3l_lvds_pm_ops,
				 rzg3l_lvds_pm_runtime_suspend,
				 rzg3l_lvds_pm_runtime_resume, NULL);

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int rzg3l_lvds_probe(struct platform_device *pdev)
{
	struct reset_control *rstc, *arstc;
	struct device *dev = &pdev->dev;
	struct rzg3l_lvds *lvds;
	int ret;

	lvds = devm_drm_bridge_alloc(dev, struct rzg3l_lvds, bridge,
				     &rzg3l_lvds_bridge_ops);
	if (IS_ERR(lvds))
		return PTR_ERR(lvds);

	lvds->dev = dev;
	lvds->bridge.of_node = pdev->dev.of_node;

	lvds->regmap = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(lvds->regmap))
		return PTR_ERR(lvds->regmap);

	rstc = devm_reset_control_get_optional_exclusive(dev, "rst");
	if (IS_ERR(rstc))
		return dev_err_probe(dev, PTR_ERR(rstc), "failed to get rst\n");

	arstc = devm_reset_control_get_optional_exclusive(dev, "arst");
	if (IS_ERR(arstc))
		return dev_err_probe(dev, PTR_ERR(arstc),
				     "failed to get arst\n");

	lvds->prstc = devm_reset_control_get_shared(dev, "prst");
	if (IS_ERR(lvds->prstc))
		return dev_err_probe(dev, PTR_ERR(lvds->prstc),
				     "failed to get prst\n");

	lvds->lvd_rstc = devm_reset_control_get_shared(dev, "lvdrst");
	if (IS_ERR(lvds->lvd_rstc))
		return dev_err_probe(dev, PTR_ERR(lvds->lvd_rstc),
				     "failed to get core reset\n");

	platform_set_drvdata(pdev, lvds);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable Runtime PM\n");

	lvds->bridge.next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);
	if (IS_ERR(lvds->bridge.next_bridge))
		return dev_err_probe(dev, PTR_ERR(lvds->bridge.next_bridge),
				     "failed to get next bridge\n");

	ret = reset_control_assert(rstc);
	if (ret < 0)
		return ret;

	ret = reset_control_assert(arstc);
	if (ret < 0)
		return ret;

	ret = devm_drm_bridge_add(dev, &lvds->bridge);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register drm bridge\n");

	return ret;
}

static const struct of_device_id rzg3l_lvds_of_table[] = {
	{ .compatible = "renesas,r9a08g046-lvds" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, rzg3l_lvds_of_table);

static struct platform_driver rzg3l_lvds_platform_driver = {
	.probe		= rzg3l_lvds_probe,
	.driver		= {
		.name	= "rzg3l-lvds",
		.pm	= pm_ptr(&rzg3l_lvds_pm_ops),
		.of_match_table = rzg3l_lvds_of_table,
	},
};

module_platform_driver(rzg3l_lvds_platform_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_AUTHOR("Tommaso Merciai <tommaso.merciai.xr@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G3L LVDS Encoder Driver");
MODULE_LICENSE("GPL");
