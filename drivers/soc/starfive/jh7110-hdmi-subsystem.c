// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the StarFive JH7110 HDMI subsystem
 *
 * Copyright (c) 2025 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * This driver binds to the monolithic HDMI block and creates separate
 * logical platform devices for the HDMI Controller (bridge) and the
 * HDMI PHY (clock/phy provider), allowing them to share a single regmap
 * and breaking the probing circular dependency.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

static const struct regmap_config starfive_hdmi_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.max_register = 0x3fff,
};

static void starfive_hdmi_subsys_clk_disable(void *data)
{
	clk_disable_unprepare(data);
}

static void starfive_hdmi_subsys_rst_assert(void *data)
{
	reset_control_assert(data);
}

static int starfive_hdmi_subsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *bus_rst;
	void __iomem *regs;
	struct regmap *regmap;
	struct clk *bus_clk;
	int ret;

	/*
	 * The NoC display-bus clock and reset gate access to the whole vout
	 * register region, and this subsystem's PHY child is the first device in
	 * that region to touch registers. Bring the bus up here before
	 * populating the children; PD_VOUT is powered on by genpd through the
	 * power-domains property.
	 */
	bus_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(bus_clk))
		return dev_err_probe(dev, PTR_ERR(bus_clk),
				     "Failed to get NoC bus clock\n");

	ret = clk_prepare_enable(bus_clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable NoC bus clock\n");

	ret = devm_add_action_or_reset(dev, starfive_hdmi_subsys_clk_disable, bus_clk);
	if (ret)
		return ret;

	bus_rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(bus_rst))
		return dev_err_probe(dev, PTR_ERR(bus_rst),
				     "Failed to get NoC bus reset\n");

	ret = reset_control_deassert(bus_rst);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to deassert NoC bus reset\n");

	ret = devm_add_action_or_reset(dev, starfive_hdmi_subsys_rst_assert, bus_rst);
	if (ret)
		return ret;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	regmap = devm_regmap_init_mmio(dev, regs,
				       &starfive_hdmi_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to init shared regmap\n");

	ret = devm_of_platform_populate(dev);
	if (ret)
		dev_err(dev, "Failed to populate child devices: %d\n", ret);

	return ret;
}

static const struct of_device_id starfive_hdmi_subsys_of_match[] = {
	{ .compatible = "starfive,jh7110-hdmi-subsystem", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, starfive_hdmi_subsys_of_match);

static struct platform_driver starfive_hdmi_subsys_driver = {
	.probe = starfive_hdmi_subsys_probe,
	.driver = {
		.name = "starfive-hdmi-subsystem",
		.of_match_table = starfive_hdmi_subsys_of_match,
	},
};
module_platform_driver(starfive_hdmi_subsys_driver);

MODULE_AUTHOR("Michal Wilczynski <m.wilczynski@samsung.com>");
MODULE_DESCRIPTION("StarFive JH7110 HDMI subsystem Driver");
MODULE_LICENSE("GPL");
