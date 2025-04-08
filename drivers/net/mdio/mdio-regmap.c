// SPDX-License-Identifier: GPL-2.0-or-later
/* Driver for MMIO-Mapped MDIO devices. Some IPs expose internal PHYs or PCS
 * within the MMIO-mapped area
 *
 * Copyright (C) 2023 Maxime Chevallier <maxime.chevallier@bootlin.com>
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mdio/mdio-regmap.h>

#define MDIO_REGMAP_C45			BIT(26)
#define MDIO_REGMAP_ADDR		GENMASK(25, 21)
#define MDIO_REGMAP_DEVNUM		GENMASK(20, 16)
#define MDIO_REGMAP_REGNUM		GENMASK(15, 0)

#define DRV_NAME "mdio-regmap"

struct mdio_regmap_priv {
	void *ctx;

	const struct mdio_regmap_init_config *config;
};

struct mdio_regmap_mii_priv {
	struct regmap *regmap;
	u32 valid_addr_mask;
	bool encode_addr;
};

static int mdio_regmap_mii_read_c22(struct mii_bus *bus, int addr, int regnum)
{
	struct mdio_regmap_mii_priv *ctx = bus->priv;
	unsigned int val;
	int ret;

	if (!(ctx->valid_addr_mask & BIT(addr)))
		return -ENODEV;

	if (ctx->encode_addr)
		regnum |= FIELD_PREP(MDIO_REGMAP_ADDR, addr);

	ret = regmap_read(ctx->regmap, regnum, &val);
	if (ret < 0)
		return ret;

	return val;
}

static int mdio_regmap_mii_write_c22(struct mii_bus *bus, int addr, int regnum,
				     u16 val)
{
	struct mdio_regmap_mii_priv *ctx = bus->priv;

	if (!(ctx->valid_addr_mask & BIT(addr)))
		return -ENODEV;

	if (ctx->encode_addr)
		regnum |= FIELD_PREP(MDIO_REGMAP_ADDR, addr);

	return regmap_write(ctx->regmap, regnum, val);
}

static int mdio_regmap_mii_read_c45(struct mii_bus *bus, int addr, int devnum,
				    int regnum)
{
	struct mdio_regmap_mii_priv *ctx = bus->priv;
	unsigned int val;
	int ret;

	if (!(ctx->valid_addr_mask & BIT(addr)))
		return -ENODEV;

	regnum |= MDIO_REGMAP_C45;
	regnum |= FIELD_PREP(MDIO_REGMAP_ADDR, addr);
	regnum |= FIELD_PREP(MDIO_REGMAP_DEVNUM, devnum);

	ret = regmap_read(ctx->regmap, regnum, &val);
	if (ret < 0)
		return ret;

	return val;
}

static int mdio_regmap_mii_write_c45(struct mii_bus *bus, int addr, int devnum,
				     int regnum, u16 val)
{
	struct mdio_regmap_mii_priv *ctx = bus->priv;

	if (!(ctx->valid_addr_mask & BIT(addr)))
		return -ENODEV;

	regnum |= MDIO_REGMAP_C45;
	regnum |= FIELD_PREP(MDIO_REGMAP_ADDR, addr);
	regnum |= FIELD_PREP(MDIO_REGMAP_DEVNUM, devnum);

	return regmap_write(ctx->regmap, regnum, val);
}

struct mii_bus *devm_mdio_regmap_register(struct device *dev,
					  const struct mdio_regmap_config *config)
{
	struct mdio_regmap_mii_priv *mr;
	struct mii_bus *mii;
	int rc;

	if (!config->parent)
		return ERR_PTR(-EINVAL);

	if (config->valid_addr_mask && !config->support_encoded_addr) {
		dev_err(dev, "encoded address support is required to support multiple MDIO address\n");
		return ERR_PTR(-EINVAL);
	}

	mii = devm_mdiobus_alloc_size(config->parent, sizeof(*mr));
	if (!mii)
		return ERR_PTR(-ENOMEM);

	mr = mii->priv;
	mr->regmap = config->regmap;
	mr->valid_addr_mask = config->valid_addr_mask ? config->valid_addr_mask :
							BIT(config->valid_addr);
	mr->encode_addr = config->support_encoded_addr;

	mii->name = DRV_NAME;
	strscpy(mii->id, config->name, MII_BUS_ID_SIZE);
	mii->parent = config->parent;
	mii->read = mdio_regmap_mii_read_c22;
	mii->write = mdio_regmap_mii_write_c22;
	if (config->support_encoded_addr) {
		mii->read_c45 = mdio_regmap_mii_read_c45;
		mii->write_c45 = mdio_regmap_mii_write_c45;
	}

	if (config->autoscan)
		mii->phy_mask = ~mr->valid_addr_mask;
	else
		mii->phy_mask = ~0;

	rc = devm_of_mdiobus_register(dev, mii, config->np);
	if (rc) {
		dev_err(config->parent, "Cannot register MDIO bus![%s] (%d)\n", mii->id, rc);
		return ERR_PTR(rc);
	}

	return mii;
}
EXPORT_SYMBOL_GPL(devm_mdio_regmap_register);

static int mdio_regmap_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	const struct mdio_regmap_init_config *config;
	struct mdio_regmap_priv *priv = context;
	int addr, regnum;
	int ret;

	config = priv->config;

	addr = FIELD_GET(MDIO_REGMAP_ADDR, reg);
	regnum = FIELD_GET(MDIO_REGMAP_REGNUM, reg);

	if (reg & MDIO_REGMAP_C45) {
		int devnum;

		if (!config->mdio_write_c45)
			return -EOPNOTSUPP;

		devnum = FIELD_GET(MDIO_REGMAP_DEVNUM, reg);
		ret = config->mdio_read_c45(priv->ctx, addr, devnum, regnum);
	} else {
		ret = config->mdio_read(priv->ctx, addr, regnum);
	}

	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int mdio_regmap_reg_write(void *context, unsigned int reg, unsigned int val)
{
	const struct mdio_regmap_init_config *config;
	struct mdio_regmap_priv *priv = context;
	int addr, regnum;

	config = priv->config;

	addr = FIELD_GET(MDIO_REGMAP_ADDR, reg);
	regnum = FIELD_GET(MDIO_REGMAP_REGNUM, reg);

	if (reg & MDIO_REGMAP_C45) {
		int devnum;

		if (!config->mdio_write_c45)
			return -EOPNOTSUPP;

		devnum = FIELD_GET(MDIO_REGMAP_DEVNUM, reg);
		return config->mdio_write_c45(priv->ctx, addr, devnum, regnum, val);
	}

	return config->mdio_write(priv->ctx, addr, regnum, val);
}

static const struct regmap_config mdio_regmap_default_config = {
	.reg_bits = 26,
	.val_bits = 16,
	.reg_stride = 1,
	.max_register = MDIO_REGMAP_C45 | MDIO_REGMAP_ADDR |
			MDIO_REGMAP_DEVNUM | MDIO_REGMAP_REGNUM,
	.reg_read = mdio_regmap_reg_read,
	.reg_write = mdio_regmap_reg_write,
	/* Locking MUST be handled in mdio_write/read(_c45) */
	.disable_locking = true,
};

struct regmap *devm_mdio_regmap_init(struct device *dev, void *priv,
				     const struct mdio_regmap_init_config *config)
{
	struct mdio_regmap_priv *mdio_regmap_priv;
	struct regmap_config regmap_config;

	/* Validate config */
	if (!config->mdio_read || !config->mdio_write) {
		dev_err(dev, ".mdio_read and .mdio_write MUST be defined in config\n");
		return ERR_PTR(-EINVAL);
	}

	mdio_regmap_priv = devm_kzalloc(dev, sizeof(*mdio_regmap_priv),
					GFP_KERNEL);
	if (!mdio_regmap_priv)
		return ERR_PTR(-ENOMEM);

	memcpy(&regmap_config, &mdio_regmap_default_config, sizeof(regmap_config));
	regmap_config.name = config->name;

	mdio_regmap_priv->ctx = priv;
	mdio_regmap_priv->config = config;

	return devm_regmap_init(dev, NULL, mdio_regmap_priv,
				&regmap_config);
}
EXPORT_SYMBOL_GPL(devm_mdio_regmap_init);

MODULE_DESCRIPTION("MDIO API over regmap");
MODULE_AUTHOR("Maxime Chevallier <maxime.chevallier@bootlin.com>");
MODULE_LICENSE("GPL");
