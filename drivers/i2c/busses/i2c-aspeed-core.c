// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASPEED I2C core driver
 *
 * Copyright (C) ASPEED Technology Inc.
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include "i2c-aspeed-core.h"

struct aspeed_i2c_core_priv {
	void (*remove)(struct platform_device *pdev);
	void *bus_data;
};

static const struct of_device_id aspeed_i2c_of_match[] = {
	{
		.compatible = "aspeed,ast2400-i2c-bus",
		.data = (const void *)AST2400_I2C
	},
	{
		.compatible = "aspeed,ast2500-i2c-bus",
		.data = (const void *)AST2500_I2C
	},
	{
		.compatible = "aspeed,ast2600-i2c-bus",
		.data = (const void *)AST2600_I2C
	},
	{ }
};

MODULE_DEVICE_TABLE(of, aspeed_i2c_of_match);

static int aspeed_i2c_core_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aspeed_i2c_core_priv *priv;
	const struct of_device_id *match;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	match = of_match_device(aspeed_i2c_of_match, dev);
	if (!match)
		return -ENODEV;

	if (device_is_compatible(dev, "aspeed,ast2600-i2c-bus") &&
	    device_property_present(dev, "aspeed,global-regs")) {
		ret = ast2600_i2c_probe(match, pdev);
		priv->remove = ast2600_i2c_remove;
	} else {
		ret = aspeed_i2c_probe_bus(match, pdev);
		priv->remove = aspeed_i2c_remove_bus;
	}

	priv->bus_data = platform_get_drvdata(pdev);
	platform_set_drvdata(pdev, priv);
	return ret;
}

static void aspeed_i2c_core_remove(struct platform_device *pdev)
{
	struct aspeed_i2c_core_priv *priv = platform_get_drvdata(pdev);

	if (!priv || !priv->remove)
		return;

	platform_set_drvdata(pdev, priv->bus_data);
	return priv->remove(pdev);
}

static struct platform_driver aspeed_i2c_driver = {
	.probe  = aspeed_i2c_core_probe,
	.remove = aspeed_i2c_core_remove,
	.driver = {
		.name           = "i2c-aspeed-core",
		.of_match_table = aspeed_i2c_of_match,
	},
};
module_platform_driver(aspeed_i2c_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("Unified ASPEED I2C driver (AST24xx/AST25xx/AST2600)");
MODULE_LICENSE("GPL");
