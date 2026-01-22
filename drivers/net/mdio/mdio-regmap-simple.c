// SPDX-License-Identifier: GPL-2.0
/* Copyright 2025-2026 NXP
 *
 * Generic MDIO bus driver for simple regmap-based MDIO devices
 *
 * This driver creates MDIO buses for devices that expose their internal
 * PHYs or PCS through a regmap interface. It's intended to be a simple,
 * generic driver similar to simple-mfd-i2c.c.
 */
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mdio/mdio-regmap.h>

struct mdio_regmap_simple_data {
	u8 valid_addr;
	bool autoscan;
};

static const struct mdio_regmap_simple_data nxp_sja1110_base_tx = {
	.valid_addr = 0,
	.autoscan = false,
};

static int mdio_regmap_simple_probe(struct platform_device *pdev)
{
	const struct mdio_regmap_simple_data *data;
	struct mdio_regmap_config config = {};
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct mii_bus *bus;

	if (!dev->of_node || !dev->parent)
		return -ENODEV;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	data = device_get_match_data(dev);

	config.regmap = regmap;
	config.parent = dev;
	config.name = dev_name(dev);
	/* The resource is optional, provided for finding the registers
	 * within a device-wide non-MMIO regmap
	 */
	config.resource = platform_get_resource(pdev, IORESOURCE_REG, 0);
	if (data) {
		config.valid_addr = data->valid_addr;
		config.autoscan = data->autoscan;
	}

	return PTR_ERR_OR_ZERO(devm_mdio_regmap_register(dev, &config));
}

static const struct of_device_id mdio_regmap_simple_match[] = {
	{
		.compatible = "nxp,sja1110-base-tx-mdio",
		.data = &nxp_sja1110_base_tx,
	},
	{}
};
MODULE_DEVICE_TABLE(of, mdio_regmap_simple_match);

static struct platform_driver mdio_regmap_simple_driver = {
	.probe = mdio_regmap_simple_probe,
	.driver = {
		.name = "mdio-regmap-simple",
		.of_match_table = mdio_regmap_simple_match,
	},
};

module_platform_driver(mdio_regmap_simple_driver);

MODULE_DESCRIPTION("Generic MDIO bus driver for simple regmap-based devices");
MODULE_AUTHOR("Vladimir Oltean <vladimir.oltean@nxp.com>");
MODULE_LICENSE("GPL");
