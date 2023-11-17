// SPDX-License-Identifier: GPL-2.0+
/* Realtek MDIO interface driver
 *
 * ASICs we intend to support with this driver:
 *
 * RTL8366   - The original version, apparently
 * RTL8369   - Similar enough to have the same datsheet as RTL8366
 * RTL8366RB - Probably reads out "RTL8366 revision B", has a quite
 *             different register layout from the other two
 * RTL8366S  - Is this "RTL8366 super"?
 * RTL8367   - Has an OpenWRT driver as well
 * RTL8368S  - Seems to be an alternative name for RTL8366RB
 * RTL8370   - Also uses SMI
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 * Copyright (C) 2010 Antti Seppälä <a.seppala@gmail.com>
 * Copyright (C) 2010 Roman Yeryomin <roman@advem.lv>
 * Copyright (C) 2011 Colin Leitner <colin.leitner@googlemail.com>
 * Copyright (C) 2009-2010 Gabor Juhos <juhosg@openwrt.org>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/regmap.h>

#include "realtek.h"
#include "realtek-common.h"

/* Read/write via mdiobus */
#define REALTEK_MDIO_CTRL0_REG		31
#define REALTEK_MDIO_START_REG		29
#define REALTEK_MDIO_CTRL1_REG		21
#define REALTEK_MDIO_ADDRESS_REG	23
#define REALTEK_MDIO_DATA_WRITE_REG	24
#define REALTEK_MDIO_DATA_READ_REG	25

#define REALTEK_MDIO_START_OP		0xFFFF
#define REALTEK_MDIO_ADDR_OP		0x000E
#define REALTEK_MDIO_READ_OP		0x0001
#define REALTEK_MDIO_WRITE_OP		0x0003

static int realtek_mdio_write(void *ctx, u32 reg, u32 val)
{
	struct realtek_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	int ret;

	mutex_lock(&bus->mdio_lock);

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	if (ret)
		goto out_unlock;

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_ADDRESS_REG, reg);
	if (ret)
		goto out_unlock;

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_DATA_WRITE_REG, val);
	if (ret)
		goto out_unlock;

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_WRITE_OP);

out_unlock:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int realtek_mdio_read(void *ctx, u32 reg, u32 *val)
{
	struct realtek_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	int ret;

	mutex_lock(&bus->mdio_lock);

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_CTRL0_REG, REALTEK_MDIO_ADDR_OP);
	if (ret)
		goto out_unlock;

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_ADDRESS_REG, reg);
	if (ret)
		goto out_unlock;

	ret = bus->write(bus, priv->mdio_addr, REALTEK_MDIO_CTRL1_REG, REALTEK_MDIO_READ_OP);
	if (ret)
		goto out_unlock;

	ret = bus->read(bus, priv->mdio_addr, REALTEK_MDIO_DATA_READ_REG);
	if (ret >= 0) {
		*val = ret;
		ret = 0;
	}

out_unlock:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static const struct regmap_config realtek_mdio_regmap_config = {
	.reg_bits = 10, /* A4..A0 R4..R0 */
	.val_bits = 16,
	.reg_stride = 1,
	/* PHY regs are at 0x8000 */
	.max_register = 0xffff,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.reg_read = realtek_mdio_read,
	.reg_write = realtek_mdio_write,
	.cache_type = REGCACHE_NONE,
	.lock = realtek_common_lock,
	.unlock = realtek_common_unlock,
};

static const struct regmap_config realtek_mdio_nolock_regmap_config = {
	.reg_bits = 10, /* A4..A0 R4..R0 */
	.val_bits = 16,
	.reg_stride = 1,
	/* PHY regs are at 0x8000 */
	.max_register = 0xffff,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.reg_read = realtek_mdio_read,
	.reg_write = realtek_mdio_write,
	.cache_type = REGCACHE_NONE,
	.disable_locking = true,
};

static int realtek_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct realtek_priv *priv;
	int ret;

	priv = realtek_common_probe(dev, realtek_mdio_regmap_config,
				    realtek_mdio_nolock_regmap_config);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->mdio_addr = mdiodev->addr;
	priv->bus = mdiodev->bus;
	priv->write_reg_noack = realtek_mdio_write;

	ret = priv->ops->detect(priv);
	if (ret) {
		dev_err(dev, "unable to detect switch\n");
		goto err_variant_put;
	}

	priv->ds->ops = priv->variant->ds_ops_mdio;
	priv->ds->num_ports = priv->num_ports;

	ret = dsa_register_switch(priv->ds);
	if (ret) {
		dev_err_probe(dev, ret, "unable to register switch\n");
		goto err_variant_put;
	}

	return 0;

err_variant_put:
	realtek_variant_put(priv->variant);

	return ret;
}

static void realtek_mdio_remove(struct mdio_device *mdiodev)
{
	struct realtek_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (!priv)
		return;

	realtek_common_remove(priv);
}

static void realtek_mdio_shutdown(struct mdio_device *mdiodev)
{
	struct realtek_priv *priv = dev_get_drvdata(&mdiodev->dev);

	if (!priv)
		return;

	dsa_switch_shutdown(priv->ds);

	dev_set_drvdata(&mdiodev->dev, NULL);
}

static struct mdio_driver realtek_mdio_driver = {
	.mdiodrv.driver = {
		.name = "realtek-mdio",
		.of_match_table = realtek_common_of_match,
	},
	.probe  = realtek_mdio_probe,
	.remove = realtek_mdio_remove,
	.shutdown = realtek_mdio_shutdown,
};

mdio_module_driver(realtek_mdio_driver);

MODULE_AUTHOR("Luiz Angelo Daros de Luca <luizluca@gmail.com>");
MODULE_DESCRIPTION("Driver for Realtek ethernet switch connected via MDIO interface");
MODULE_LICENSE("GPL");
