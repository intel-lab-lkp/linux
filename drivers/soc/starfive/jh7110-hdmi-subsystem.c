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

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static const struct regmap_config starfive_hdmi_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.max_register = 0x3fff,
};

static int starfive_hdmi_subsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	void __iomem *regs;
	int ret;

	/*
	 * The NoC display bus clock and reset that gate access to this region,
	 * and the PD_VOUT power domain it sits in, are held by the video
	 * output subsystem parent for as long as this device exists.
	 */
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
		return dev_err_probe(dev, ret,
				     "Failed to populate child devices\n");

	return 0;
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
