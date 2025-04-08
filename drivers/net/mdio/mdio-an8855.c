// SPDX-License-Identifier: GPL-2.0+
/*
 * MDIO passthrough driver for Airoha AN8855 Switch
 */

#include <linux/mdio/mdio-regmap.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static int an8855_mdio_probe(struct platform_device *pdev)
{
	struct mdio_regmap_config mrc = { };
	struct device *dev = &pdev->dev;
	struct mii_bus *bus;

	mrc.regmap = dev_get_regmap(dev->parent, "phy");
	mrc.parent = dev;
	mrc.valid_addr_mask = GENMASK(31, 0);
	mrc.support_encoded_addr = true;
	mrc.autoscan = true;
	mrc.np = dev->of_node;
	snprintf(mrc.name, MII_BUS_ID_SIZE, KBUILD_MODNAME);

	bus = devm_mdio_regmap_register(dev, &mrc);
	if (IS_ERR(bus))
		return dev_err_probe(dev, PTR_ERR(bus), "failed to register MDIO bus\n");

	return 0;
}

static const struct of_device_id an8855_mdio_of_match[] = {
	{ .compatible = "airoha,an8855-mdio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, an8855_mdio_of_match);

static struct platform_driver an8855_mdio_driver = {
	.probe	= an8855_mdio_probe,
	.driver = {
		.name = "an8855-mdio",
		.of_match_table = an8855_mdio_of_match,
	},
};
module_platform_driver(an8855_mdio_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Driver for AN8855 MDIO passthrough");
MODULE_LICENSE("GPL");
