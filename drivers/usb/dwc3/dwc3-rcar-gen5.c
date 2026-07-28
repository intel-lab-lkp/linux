// SPDX-License-Identifier: GPL-2.0-only
/*
 * Renesas USB device driver with DWC3 integration
 *
 * Copyright (C) 2025-2026 Renesas Electronics Corporation
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

struct dwc3_rcar_gen5_priv {
	void __iomem		*base;
	struct clk		*clk;
	bool			use_usb3_flow;
};

static int dwc3_rcar_gen5_init(struct dwc3_rcar_gen5_priv *priv)
{
	/*
	 * The datasheet describes initialization procedure without full
	 * information about the registers. Therefore, the source code is
	 * based on the bare metal code shared by the board team.
	 */
	writew(0x211, priv->base + 0x26);

	/* USB3 does not need additional register programming. */
	if (priv->use_usb3_flow)
		return 0;

	writew(0x11, priv->base + 0x81c);
	writew(0x0, priv->base + 0x81a);
	writew(0x1, priv->base + 0x802);

	usleep_range(10000, 20000);

	writew(0x0, priv->base + 0x802);
	writew(0x1, priv->base + 0x2a);
	writew(0x1, priv->base + 0x81a);

	usleep_range(10000, 20000);

	return 0;
}

static int dwc3_rcar_gen5_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *subnode;
	struct reset_control *reset;
	const char *maximum_speed;
	struct dwc3_rcar_gen5_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return dev_err_probe(dev, PTR_ERR(priv->base), "Failed to map registers\n");

	reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Failed to get reset control\n");

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk), "Failed to get clock control\n");

	subnode = of_get_compatible_child(dev->of_node, "synopsys,dwc3");
	if (!subnode)
		return dev_err_probe(dev, -ENODEV, "Failed to find DWC3 subnode node\n");

	ret = of_property_read_string(subnode, "maximum-speed", &maximum_speed);
	of_node_put(subnode);
	if (ret)
		return dev_err_probe(dev, -ENODEV, "Failed to determine maximum speed\n");

	priv->use_usb3_flow = !strcmp(maximum_speed, "super-speed-plus") ||
			      !strcmp(maximum_speed, "super-speed");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable runtime PM\n");

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to resume runtime PM\n");

	return devm_of_platform_populate(dev);
}

static int __maybe_unused dwc3_rcar_gen5_suspend(struct device *dev)
{
	struct dwc3_rcar_gen5_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk);

	return 0;
}

static int __maybe_unused dwc3_rcar_gen5_resume(struct device *dev)
{
	struct dwc3_rcar_gen5_priv *priv = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock on resume: %d\n", ret);
		return ret;
	}

	usleep_range(10000, 20000);

	return dwc3_rcar_gen5_init(priv);
}

static DEFINE_RUNTIME_DEV_PM_OPS(dwc3_rcar_gen5_pm_ops,
				 dwc3_rcar_gen5_suspend,
				 dwc3_rcar_gen5_resume, NULL);

static const struct of_device_id dwc3_rcar_gen5_of_match[] = {
	{ .compatible = "renesas,rcar-gen5-usb" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dwc3_rcar_gen5_of_match);

static struct platform_driver dwc3_rcar_gen5_driver = {
	.probe		= dwc3_rcar_gen5_probe,
	.driver		= {
		.name	= "renesas-rcar-gen5-usb",
		.of_match_table = dwc3_rcar_gen5_of_match,
		.pm	= &dwc3_rcar_gen5_pm_ops,
	},
};

module_platform_driver(dwc3_rcar_gen5_driver);

MODULE_AUTHOR("Thanh Quan");
MODULE_DESCRIPTION("Renesas R-Car X5H USB Glue Layer Driver");
MODULE_LICENSE("GPL");
