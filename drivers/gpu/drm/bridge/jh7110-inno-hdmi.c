// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) StarFive Technology Co., Ltd.
 * Copyright (c) 2025 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * HDMI controller (bridge) driver for the StarFive JH7110 HDMI subsystem.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/bridge/inno_hdmi.h>
#include <drm/drm_modes.h>

/* dom_vout_syscon: HDMI pixel data mapping */
#define VOUT_SYSCFG_4			0x4
#define VOUT_HDMI_DP_BIT_DEPTH		BIT(25)
#define VOUT_HDMI_DP_YUV_MODE		GENMASK(27, 26)
#define VOUT_HDMI_DP_YUV_MODE_RGB	3
#define VOUT_HDMI_DPI_BIT_DEPTH		GENMASK(29, 28)
#define VOUT_HDMI_DPI_BIT_DEPTH_8BIT	0
#define VOUT_HDMI_DPI_DP_SEL		BIT(30)

/* u2_display_panel_mux feeds HDMI_Ctrl, see the block diagram in 5.1 */
#define VOUT_SYSCFG_8			0x8
#define VOUT_HDMI_PANEL_SEL		BIT(4)

enum stf_hdmi_ctrl_clocks { CLK_SYS = 0, CLK_M, CLK_B, CLK_PCLK, CLK_CTRL_NUM };

struct stf_inno_hdmi_controller {
	struct device *dev;
	struct clk_bulk_data clks[CLK_CTRL_NUM];
	struct reset_control *tx_rst;
	struct phy *phy;
	bool enabled;
};

static enum drm_mode_status
inno_hdmi_starfive_mode_valid(struct device *dev,
			      const struct drm_display_mode *mode)
{
	struct stf_inno_hdmi_controller *ctrl = dev_get_drvdata(dev);
	unsigned long pixelclk = mode->clock * 1000;
	long rounded;

	/*
	 * The PHY can only generate the discrete set of pixel clocks described
	 * by its pre-PLL table, and clk_round_rate() fails for anything else.
	 * Reject those modes here: without this the modeset would appear to
	 * succeed while the PHY never produces a signal.
	 */
	rounded = clk_round_rate(ctrl->clks[CLK_PCLK].clk, pixelclk);
	if (rounded < 0 || rounded != pixelclk)
		return MODE_NOCLOCK;

	return MODE_OK;
}

static void inno_hdmi_starfive_enable(struct device *dev,
				      struct drm_display_mode *mode)
{
	struct stf_inno_hdmi_controller *ctrl = dev_get_drvdata(dev);
	int ret;

	/*
	 * 1. Set the pixel clock rate. This calls the PHY driver's .set_rate op.
	 */
	ret = clk_set_rate(ctrl->clks[CLK_PCLK].clk, mode->clock * 1000);
	if (ret) {
		dev_err(dev, "Failed to set pclk rate %d: %d\n",
			mode->clock * 1000, ret);
		return;
	}

	/*
	 * 2. Enable the pixel clock. This calls the PHY driver's .prepare op.
	 */
	ret = clk_prepare_enable(ctrl->clks[CLK_PCLK].clk);
	if (ret) {
		dev_err(dev, "Failed to enable pclk: %d\n", ret);
		return;
	}

	/*
	 * 3. Power on the PHY. This calls the PHY driver's .power_on op,
	 * which configures the Post-PLL and analog blocks.
	 */
	ret = phy_power_on(ctrl->phy);
	if (ret) {
		dev_err(dev, "Failed to power on PHY: %d\n", ret);
		clk_disable_unprepare(ctrl->clks[CLK_PCLK].clk);
		return;
	}

	ctrl->enabled = true;
}

static void inno_hdmi_starfive_disable(struct device *dev)
{
	struct stf_inno_hdmi_controller *ctrl = dev_get_drvdata(dev);

	/*
	 * .enable bails out early if the pixel clock rate is unsupported or
	 * the PHY fails to power on, leaving pclk and the PHY untouched.
	 * Only tear down what was actually brought up, otherwise the clock
	 * refcount underflows.
	 */
	if (!ctrl->enabled)
		return;

	phy_power_off(ctrl->phy);
	clk_disable_unprepare(ctrl->clks[CLK_PCLK].clk);
	ctrl->enabled = false;
}

/*
 * The DC8200 has two panels, each exposing a DP and a DPI interface, and a mux
 * in dom_vout_syscon picks which of them drives the HDMI transmitter. Derive
 * the mux setting from the port graph: the remote port number selects the
 * DC8200 panel, and the remote endpoint number the interface on that panel
 * (0 for DPI, 1 for DP). Both drive 8-bit RGB, the only format this driver
 * currently produces.
 */
static int stf_inno_hdmi_setup_mux(struct device *dev)
{
	struct device_node *ep, *remote;
	struct of_endpoint endpoint;
	struct regmap *syscon;
	u32 mask, val;
	int ret;

	syscon = syscon_regmap_lookup_by_phandle(dev->of_node,
						 "starfive,vout-syscon");
	if (IS_ERR(syscon))
		return dev_err_probe(dev, PTR_ERR(syscon),
				     "Failed to get vout syscon\n");

	ep = of_graph_get_endpoint_by_regs(dev->of_node, 0, -1);
	if (!ep)
		return dev_err_probe(dev, -ENODEV, "No input endpoint\n");

	remote = of_graph_get_remote_endpoint(ep);
	of_node_put(ep);
	if (!remote)
		return dev_err_probe(dev, -ENODEV,
				     "Input endpoint is not connected\n");

	ret = of_graph_parse_endpoint(remote, &endpoint);
	of_node_put(remote);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to parse the remote endpoint\n");

	if (endpoint.port > 1 || endpoint.id > 1)
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported DC8200 output %u/%u\n",
				     endpoint.port, endpoint.id);

	/* Data mapping: 8-bit RGB on whichever interface is in use. */
	mask = VOUT_HDMI_DPI_DP_SEL | VOUT_HDMI_DP_BIT_DEPTH |
	       VOUT_HDMI_DP_YUV_MODE | VOUT_HDMI_DPI_BIT_DEPTH;
	val = FIELD_PREP(VOUT_HDMI_DPI_DP_SEL, endpoint.id) |
	      FIELD_PREP(VOUT_HDMI_DP_YUV_MODE, VOUT_HDMI_DP_YUV_MODE_RGB) |
	      FIELD_PREP(VOUT_HDMI_DPI_BIT_DEPTH, VOUT_HDMI_DPI_BIT_DEPTH_8BIT);

	ret = regmap_update_bits(syscon, VOUT_SYSCFG_4, mask, val);
	if (ret)
		return ret;

	/* Which DC8200 panel drives the HDMI transmitter. */
	return regmap_update_bits(syscon, VOUT_SYSCFG_8, VOUT_HDMI_PANEL_SEL,
				  FIELD_PREP(VOUT_HDMI_PANEL_SEL,
					     endpoint.port));
}

static void stf_inno_hdmi_clk_disable(void *data)
{
	struct stf_inno_hdmi_controller *ctrl = data;

	clk_bulk_disable_unprepare(CLK_CTRL_NUM - 1, ctrl->clks);
}

static void stf_inno_hdmi_rst_assert(void *data)
{
	reset_control_assert(data);
}

static int starfive_inno_hdmi_controller_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct stf_inno_hdmi_controller *ctrl;
	const struct inno_hdmi_plat_data *plat_data;
	struct regmap *regmap;
	struct inno_hdmi *inno;
	int ret;

	ctrl = devm_kzalloc(dev, sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	ctrl->dev = dev;
	platform_set_drvdata(pdev, ctrl);

	/* Get the shared regmap from the parent */
	regmap = dev_get_regmap(parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get parent regmap\n");
		return -ENODEV;
	}

	ctrl->phy = devm_phy_get(dev, NULL);
	if (IS_ERR(ctrl->phy))
		return dev_err_probe(dev, PTR_ERR(ctrl->phy), "Failed to get PHY\n");

	ctrl->tx_rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(ctrl->tx_rst))
		return dev_err_probe(dev, PTR_ERR(ctrl->tx_rst), "failed to get tx reset\n");

	/* Populate the clock names this controller *consumes* */
	ctrl->clks[CLK_SYS].id = "pclk";
	ctrl->clks[CLK_M].id = "mclk";
	ctrl->clks[CLK_B].id = "bclk";
	ctrl->clks[CLK_PCLK].id = "pixel"; /* Generated by the PHY */

	ret = devm_clk_bulk_get(dev, CLK_CTRL_NUM, ctrl->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to get controller clocks\n");

	/*
	 * Tear the clocks and the reset down through devm, so that they outlive
	 * everything registered after them. The bridge is added with
	 * devm_drm_bridge_add(), and unwinding in the wrong order would leave it
	 * registered while its clocks are already gated.
	 *
	 * The pixel clock is enabled on demand during modeset.
	 */
	ret = clk_bulk_prepare_enable(CLK_CTRL_NUM - 1, ctrl->clks);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, stf_inno_hdmi_clk_disable, ctrl);
	if (ret)
		return ret;

	ret = reset_control_deassert(ctrl->tx_rst);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, stf_inno_hdmi_rst_assert,
				       ctrl->tx_rst);
	if (ret)
		return ret;

	ret = stf_inno_hdmi_setup_mux(dev);
	if (ret)
		return ret;

	plat_data = of_device_get_match_data(dev);

	/* Hand off to the generic library to create the bridge. */
	inno = inno_hdmi_probe(pdev, plat_data);
	if (IS_ERR(inno))
		return PTR_ERR(inno);

	return 0;
}

/*
 * This table is now only used for the generic .mode_valid check.
 * The real validation happens in the PHY driver's .round_rate.
 */
static struct inno_hdmi_phy_config stf_hdmi_phy_configs[] = {
	{ 297000000, 0x00, 0x00 },
	{ ~0UL, 0x00, 0x00 }, /* Sentinel */
};

static const struct inno_hdmi_plat_ops stf_inno_hdmi_plat_ops = {
	.enable = inno_hdmi_starfive_enable,
	.disable = inno_hdmi_starfive_disable,
	.mode_valid = inno_hdmi_starfive_mode_valid,
};

static const struct inno_hdmi_plat_data stf_inno_hdmi_plat_data = {
	.ops = &stf_inno_hdmi_plat_ops,
	.phy_configs = stf_hdmi_phy_configs,
	.default_phy_config = &stf_hdmi_phy_configs[0],
};

static const struct of_device_id starfive_hdmi_controller_dt_ids[] = {
	{ .compatible = "starfive,jh7110-inno-hdmi-controller",
	  .data = &stf_inno_hdmi_plat_data },
	{}
};
MODULE_DEVICE_TABLE(of, starfive_hdmi_controller_dt_ids);

struct platform_driver starfive_inno_hdmi_controller_driver = {
	.probe = starfive_inno_hdmi_controller_probe,
	.driver = {
		.name = "starfive-inno-hdmi-controller",
		.of_match_table = starfive_hdmi_controller_dt_ids,
	},
};
module_platform_driver(starfive_inno_hdmi_controller_driver);

MODULE_AUTHOR("Michal Wilczynski <m.wilczynski@samsung.com>");
MODULE_DESCRIPTION("StarFive INNO HDMI Controller Driver");
MODULE_LICENSE("GPL");
