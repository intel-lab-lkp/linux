// SPDX-License-Identifier: GPL-2.0
/* Airoha AN7583 MDIO interface driver
 *
 * Copyright (C) 2025 Christian Marangi <ansuelsmth@gmail.com>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define AN7583_MAX_MDIO_BUS			2

/* MII address register definitions */
#define AN7583_MDIO0_ADDR			0xc8
#define AN7583_MDIO1_ADDR			0xcc
#define   AN7583_MII_BUSY			BIT(31)
#define   AN7583_MII_RDY			BIT(30) /* RO signal BUS is ready */
#define   AN7583_MII_CL22_REG_ADDR		GENMASK(29, 25)
#define   AN7583_MII_CL45_DEV_ADDR		AN7583_MII_CL22_REG_ADDR
#define   AN7583_MII_PHY_ADDR			GENMASK(24, 20)
#define   AN7583_MII_CMD			GENMASK(19, 18)
#define   AN7583_MII_CMD_CL22_WRITE		FIELD_PREP_CONST(AN7583_MII_CMD, 0x1)
#define   AN7583_MII_CMD_CL22_READ		FIELD_PREP_CONST(AN7583_MII_CMD, 0x2)
#define   AN7583_MII_CMD_CL45_ADDR		FIELD_PREP_CONST(AN7583_MII_CMD, 0x0)
#define   AN7583_MII_CMD_CL45_WRITE		FIELD_PREP_CONST(AN7583_MII_CMD, 0x1)
#define   AN7583_MII_CMD_CL45_POSTREAD_INCADDR	FIELD_PREP_CONST(AN7583_MII_CMD, 0x2)
#define   AN7583_MII_CMD_CL45_READ		FIELD_PREP_CONST(AN7583_MII_CMD, 0x3)
#define   AN7583_MII_ST				GENMASK(17, 16)
#define   AN7583_MII_ST_CL45			FIELD_PREP_CONST(AN7583_MII_ST, 0x0)
#define   AN7583_MII_ST_CL22			FIELD_PREP_CONST(AN7583_MII_ST, 0x1)
#define   AN7583_MII_RWDATA			GENMASK(15, 0)
#define   AN7583_MII_CL45_REG_ADDR		AN7583_MII_RWDATA
#define AN7583_MDIO_PHY				0xd4
#define   AN7583_MDIO1_SPEED_MODE		BIT(11)
#define   AN7583_MDIO0_SPEED_MODE		BIT(10)

#define AN7583_MII_MDIO_DELAY_USEC		100
#define AN7583_MII_MDIO_RETRY_MSEC		100

static const u32 airoha_mdio_bus_base_addrs[] = {
	AN7583_MDIO0_ADDR,
	AN7583_MDIO1_ADDR,
};

static const u32 airoha_mdio_bus_speed_mode[] = {
	AN7583_MDIO0_SPEED_MODE,
	AN7583_MDIO1_SPEED_MODE,
};

struct airoha_mdio_data {
	u32 base_addr;
	struct regmap *regmap;
	struct reset_control *reset;
};

static int
airoha_mdio_wait_busy(struct airoha_mdio_data *priv)
{
	u32 busy;

	return regmap_read_poll_timeout(priv->regmap, priv->base_addr, busy,
					!(busy & AN7583_MII_BUSY),
					AN7583_MII_MDIO_DELAY_USEC,
					AN7583_MII_MDIO_RETRY_MSEC * USEC_PER_MSEC);
}

static int airoha_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct airoha_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL22 |
	      AN7583_MII_CMD_CL22_READ;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL22_REG_ADDR, regnum);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, priv->base_addr, &val);
	if (ret)
		return ret;

	return FIELD_GET(AN7583_MII_RWDATA, val);
}

static int airoha_mdio_write(struct mii_bus *bus, int addr, int regnum,
			     u16 value)
{
	struct airoha_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL22 |
	      AN7583_MII_CMD_CL22_WRITE;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL22_REG_ADDR, regnum);
	val |= FIELD_PREP(AN7583_MII_RWDATA, value);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);

	return ret;
}

static int airoha_mdio_cl45_read(struct mii_bus *bus, int addr, int devnum,
				 int regnum)
{
	struct airoha_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL45 |
	      AN7583_MII_CMD_CL45_ADDR;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL45_DEV_ADDR, devnum);
	val |= FIELD_PREP(AN7583_MII_CL45_REG_ADDR, regnum);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);
	if (ret)
		return ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL45 |
	      AN7583_MII_CMD_CL45_READ;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL45_DEV_ADDR, devnum);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, priv->base_addr, &val);
	if (ret)
		return ret;

	return FIELD_GET(AN7583_MII_RWDATA, val);
}

static int airoha_mdio_cl45_write(struct mii_bus *bus, int addr, int devnum,
				  int regnum, u16 value)
{
	struct airoha_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL45 |
	      AN7583_MII_CMD_CL45_ADDR;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL45_DEV_ADDR, devnum);
	val |= FIELD_PREP(AN7583_MII_CL45_REG_ADDR, regnum);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);
	if (ret)
		return ret;

	val = AN7583_MII_BUSY | AN7583_MII_ST_CL45 |
	      AN7583_MII_CMD_CL45_WRITE;
	val |= FIELD_PREP(AN7583_MII_PHY_ADDR, addr);
	val |= FIELD_PREP(AN7583_MII_CL45_DEV_ADDR, devnum);
	val |= FIELD_PREP(AN7583_MII_RWDATA, value);

	ret = regmap_write(priv->regmap, priv->base_addr, val);
	if (ret)
		return ret;

	ret = airoha_mdio_wait_busy(priv);

	return ret;
}

static int airoha_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_mdio_data *priv;
	struct mii_bus *bus;
	int ret;

	if (of_get_child_count(dev->of_node) > AN7583_MAX_MDIO_BUS)
		return -EINVAL;

	for_each_child_of_node_scoped(dev->of_node, child) {
		u32 id;

		ret = of_property_read_u32(child, "reg", &id);
		if (ret)
			return ret;

		if (id > AN7583_MAX_MDIO_BUS)
			return -EINVAL;

		bus = devm_mdiobus_alloc_size(dev, sizeof(*priv));
		if (!bus)
			return -ENOMEM;

		priv = bus->priv;
		priv->base_addr = airoha_mdio_bus_base_addrs[id];
		priv->regmap = device_node_to_regmap(dev->parent->of_node);
		priv->reset = devm_reset_control_get_optional(&pdev->dev, NULL);
		if (IS_ERR(priv->reset))
			return PTR_ERR(priv->reset);

		reset_control_deassert(priv->reset);

		bus->name = "airoha_mdio_bus";
		snprintf(bus->id, MII_BUS_ID_SIZE, "%s-%d-mii",
			 dev_name(dev), id);
		bus->parent = dev;
		bus->read = airoha_mdio_read;
		bus->write = airoha_mdio_write;
		bus->read_c45 = airoha_mdio_cl45_read;
		bus->write_c45 = airoha_mdio_cl45_write;

		ret = devm_of_mdiobus_register(dev, bus, child);
		if (ret) {
			reset_control_assert(priv->reset);
			return ret;
		}

		ret = regmap_set_bits(priv->regmap, AN7583_MDIO_PHY,
				      airoha_mdio_bus_speed_mode[id]);
		if (ret) {
			reset_control_assert(priv->reset);
			return ret;
		}
	}

	return 0;
}

static const struct of_device_id airoha_mdio_dt_ids[] = {
	{ .compatible = "airoha,an7583-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_mdio_dt_ids);

static struct platform_driver airoha_mdio_driver = {
	.probe = airoha_mdio_probe,
	.driver = {
		.name = "airoha-mdio",
		.of_match_table = airoha_mdio_dt_ids,
	},
};

module_platform_driver(airoha_mdio_driver);

MODULE_DESCRIPTION("Airoha AN7583 MDIO interface driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");
