// SPDX-License-Identifier: GPL-2.0+
/*
 * Core driver for Airoha AN8855 Switch
 */

#include <linux/bitfield.h>
#include <linux/dsa/an8855.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/core.h>
#include <linux/mdio.h>
#include <linux/mdio/mdio-regmap.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/regmap.h>

/* Register for HW trap status */
#define AN8855_HWTRAP			0x1000009c

#define AN8855_CREV			0x10005000
#define   AN8855_ID			0x8855

#define AN8855_RG_GPHY_AFE_PWD		0x1028c840

/* MII Registers */
#define AN8855_PHY_SELECT_PAGE		0x1f
#define   AN8855_PHY_PAGE		GENMASK(2, 0)
#define   AN8855_PHY_PAGE_STANDARD	FIELD_PREP_CONST(AN8855_PHY_PAGE, 0x0)
#define   AN8855_PHY_PAGE_EXTENDED_1	FIELD_PREP_CONST(AN8855_PHY_PAGE, 0x1)
#define   AN8855_PHY_PAGE_EXTENDED_4	FIELD_PREP_CONST(AN8855_PHY_PAGE, 0x4)

/* MII Registers Page 4 */
#define AN8855_PBUS_MODE		0x10
#define   AN8855_PBUS_MODE_ADDR_FIXED	0x0
#define   AN8855_PBUS_MODE_ADDR_INCR	BIT(15)
#define AN8855_PBUS_WR_ADDR_HIGH	0x11
#define AN8855_PBUS_WR_ADDR_LOW		0x12
#define AN8855_PBUS_WR_DATA_HIGH	0x13
#define AN8855_PBUS_WR_DATA_LOW		0x14
#define AN8855_PBUS_RD_ADDR_HIGH	0x15
#define AN8855_PBUS_RD_ADDR_LOW		0x16
#define AN8855_PBUS_RD_DATA_HIGH	0x17
#define AN8855_PBUS_RD_DATA_LOW		0x18

struct an8855_core_priv {
	struct mii_bus *bus;

	unsigned int switch_addr;
};

static const struct mfd_cell an8855_core_childs[] = {
	{
		.name = "an8855-efuse",
		.of_compatible = "airoha,an8855-efuse",
	}, {
		.name = "an8855-switch",
		.of_compatible = "airoha,an8855-switch",
	}, {
		.name = "an8855-mdio",
		.of_compatible = "airoha,an8855-mdio",
	}
};

static bool an8855_is_pbus_bmcr_reg(u32 reg)
{
	if ((reg & ~(AN8855_GPHY_PORT | AN8855_ADDR)) != AN8855_GPHY_ACCESS)
		return false;

	if ((reg & AN8855_ADDR) != FIELD_PREP_CONST(AN8855_CL22_ADDR,
						    MII_BMSR))
		return false;

	return true;
}

/* PHY page is Global for every Switch PHY.
 * Configure it to 4 (as Switch PAGE) and keep it that way.
 * Page selection doesn't affect the first PHY address from 0x0 to
 * 0xf and we use PBUS to access the PHY address.
 */
static int an8855_mii_set_page(struct an8855_core_priv *priv, u8 addr,
			       u8 page) __must_hold(&priv->bus->mdio_lock)
{
	struct mii_bus *bus = priv->bus;
	int ret;

	ret = __mdiobus_write(bus, addr, AN8855_PHY_SELECT_PAGE, page);
	if (ret)
		dev_err_ratelimited(&bus->dev, "failed to set mii page\n");

	return ret;
}

static int an8855_mii_read32(struct mii_bus *bus, u8 phy_id, u32 reg,
			     u32 *val) __must_hold(&bus->mdio_lock)
{
	int lo, hi, ret;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_MODE,
			      AN8855_PBUS_MODE_ADDR_FIXED);
	if (ret)
		goto err;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_RD_ADDR_HIGH,
			      upper_16_bits(reg));
	if (ret)
		goto err;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_RD_ADDR_LOW,
			      lower_16_bits(reg));
	if (ret)
		goto err;

	hi = __mdiobus_read(bus, phy_id, AN8855_PBUS_RD_DATA_HIGH);
	if (hi < 0) {
		ret = hi;
		goto err;
	}

	lo = __mdiobus_read(bus, phy_id, AN8855_PBUS_RD_DATA_LOW);
	if (lo < 0) {
		ret = lo;
		goto err;
	}

	*val = ((u16)hi << 16) | ((u16)lo & 0xffff);

	return 0;
err:
	dev_err_ratelimited(&bus->dev, "failed to read register\n");
	return ret;
}

static int an8855_regmap_read(void *ctx, uint32_t reg, uint32_t *val)
{
	struct an8855_core_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	u16 addr = priv->switch_addr;
	int ret;

	/* Workaround a HW BUG where using only PBUS for
	 * accessing internal PHY register cause the port status
	 * to not be correctly detected. It seems BMSR is required
	 * to go through direct MDIO read or is never refreshed.
	 *
	 * A theory about this is that PHY sideband signal is
	 * checked only with MDIO operation on BMSR and using
	 * PBUS doesn't trigger the check.
	 *
	 * Using interrupt to detect Link Up might be possible
	 * but it's considered an optional feature for the Switch
	 * reference (hence there could be devices with the
	 * interrupt line not connected)
	 */
	if (an8855_is_pbus_bmcr_reg(reg)) {
		addr += FIELD_GET(AN8855_GPHY_PORT, reg);
		*val = mdiobus_read(bus, addr, MII_BMSR);
		return 0;
	}

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
	ret = an8855_mii_set_page(priv, addr, AN8855_PHY_PAGE_EXTENDED_4);
	if (ret < 0)
		goto exit;

	ret = an8855_mii_read32(bus, addr, reg, val);

exit:
	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static int an8855_mii_write32(struct mii_bus *bus, u8 phy_id, u32 reg,
			      u32 val) __must_hold(&bus->mdio_lock)
{
	int ret;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_MODE,
			      AN8855_PBUS_MODE_ADDR_FIXED);
	if (ret)
		goto err;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_WR_ADDR_HIGH,
			      upper_16_bits(reg));
	if (ret)
		goto err;
	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_WR_ADDR_LOW,
			      lower_16_bits(reg));
	if (ret)
		goto err;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_WR_DATA_HIGH,
			      upper_16_bits(val));
	if (ret)
		goto err;

	ret = __mdiobus_write(bus, phy_id, AN8855_PBUS_WR_DATA_LOW,
			      lower_16_bits(val));
	if (ret)
		goto err;

	return 0;
err:
	dev_err_ratelimited(&bus->dev,
			    "failed to write an8855 register\n");
	return ret;
}

static int an8855_regmap_write(void *ctx, uint32_t reg, uint32_t val)
{
	struct an8855_core_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	u16 addr = priv->switch_addr;
	int ret;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
	ret = an8855_mii_set_page(priv, addr, AN8855_PHY_PAGE_EXTENDED_4);
	if (ret)
		goto exit;

	ret = an8855_mii_write32(bus, addr, reg, val);

exit:
	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static int an8855_regmap_update_bits(void *ctx, uint32_t reg, uint32_t mask,
				     uint32_t write_val)
{
	struct an8855_core_priv *priv = ctx;
	struct mii_bus *bus = priv->bus;
	u16 addr = priv->switch_addr;
	u32 val;
	int ret;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);
	ret = an8855_mii_set_page(priv, addr, AN8855_PHY_PAGE_EXTENDED_4);
	if (ret)
		goto exit;

	ret = an8855_mii_read32(bus, addr, reg, &val);
	if (ret < 0)
		goto exit;

	val &= ~mask;
	val |= write_val;
	ret = an8855_mii_write32(bus, addr, reg, val);

exit:
	mutex_unlock(&bus->mdio_lock);

	return ret < 0 ? ret : 0;
}

static const struct regmap_range an8855_readable_ranges[] = {
	regmap_reg_range(0x10000000, 0x10000fff), /* SCU */
	regmap_reg_range(0x10001000, 0x10001fff), /* RBUS */
	regmap_reg_range(0x10002000, 0x10002fff), /* MCU */
	regmap_reg_range(0x10005000, 0x10005fff), /* SYS SCU */
	regmap_reg_range(0x10007000, 0x10007fff), /* I2C Slave */
	regmap_reg_range(0x10008000, 0x10008fff), /* I2C Master */
	regmap_reg_range(0x10009000, 0x10009fff), /* PDMA */
	regmap_reg_range(0x1000a100, 0x1000a2ff), /* General Purpose Timer */
	regmap_reg_range(0x1000a200, 0x1000a2ff), /* GPU timer */
	regmap_reg_range(0x1000a300, 0x1000a3ff), /* GPIO */
	regmap_reg_range(0x1000a400, 0x1000a5ff), /* EFUSE */
	regmap_reg_range(0x1000c000, 0x1000cfff), /* GDMP CSR */
	regmap_reg_range(0x10010000, 0x1001ffff), /* GDMP SRAM */
	regmap_reg_range(0x10200000, 0x10203fff), /* Switch - ARL Global */
	regmap_reg_range(0x10204000, 0x10207fff), /* Switch - BMU */
	regmap_reg_range(0x10208000, 0x1020bfff), /* Switch - ARL Port */
	regmap_reg_range(0x1020c000, 0x1020cfff), /* Switch - SCH */
	regmap_reg_range(0x10210000, 0x10213fff), /* Switch - MAC */
	regmap_reg_range(0x10214000, 0x10217fff), /* Switch - MIB */
	regmap_reg_range(0x10218000, 0x1021bfff), /* Switch - Port Control */
	regmap_reg_range(0x1021c000, 0x1021ffff), /* Switch - TOP */
	regmap_reg_range(0x10220000, 0x1022ffff), /* SerDes */
	regmap_reg_range(0x10286000, 0x10286fff), /* RG Batcher */
	regmap_reg_range(0x1028c000, 0x1028ffff), /* ETHER_SYS */
	regmap_reg_range(0x30000000, 0x37ffffff), /* I2C EEPROM */
	regmap_reg_range(0x38000000, 0x3fffffff), /* BOOT_ROM */
	regmap_reg_range(0xa0000000, 0xbfffffff), /* GPHY */
};

static const struct regmap_access_table an8855_readable_table = {
	.yes_ranges = an8855_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(an8855_readable_ranges),
};

static const struct regmap_config an8855_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xbfffffff,
	.reg_read = an8855_regmap_read,
	.reg_write = an8855_regmap_write,
	.reg_update_bits = an8855_regmap_update_bits,
	.disable_locking = true,
	.rd_table = &an8855_readable_table,
};

static int an8855_read_switch_id(struct device *dev, struct regmap *regmap)
{
	u32 id;
	int ret;

	ret = regmap_read(regmap, AN8855_CREV, &id);
	if (ret)
		return ret;

	if (id != AN8855_ID) {
		dev_err(dev, "Detected Switch ID %x but %x was expected\n",
			id, AN8855_ID);
		return -ENODEV;
	}

	return 0;
}

static int an8855_core_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct an8855_core_priv *priv;
	struct gpio_desc *reset_gpio;
	struct regmap *regmap;
	u32 val;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->bus = mdiodev->bus;
	priv->switch_addr = mdiodev->addr;
	/* No DMA for mdiobus, mute warning for DMA mask not set */
	dev->dma_mask = &dev->coherent_dma_mask;

	regmap = devm_regmap_init(dev, NULL, priv, &an8855_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "regmap initialization failed\n");

	reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	if (reset_gpio) {
		usleep_range(100000, 150000);
		gpiod_set_value_cansleep(reset_gpio, 0);
		usleep_range(100000, 150000);
		gpiod_set_value_cansleep(reset_gpio, 1);

		/* Poll HWTRAP reg to wait for Switch to fully Init */
		ret = regmap_read_poll_timeout(regmap, AN8855_HWTRAP, val,
					       val, 20, 200000);
		if (ret)
			return ret;
	}

	ret = an8855_read_switch_id(dev, regmap);
	if (ret)
		return ret;

	/* Release global PHY power down */
	ret = regmap_write(regmap, AN8855_RG_GPHY_AFE_PWD, 0x0);
	if (ret)
		return ret;

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, an8855_core_childs,
				    ARRAY_SIZE(an8855_core_childs), NULL, 0,
				    NULL);
}

static const struct of_device_id an8855_core_of_match[] = {
	{ .compatible = "airoha,an8855" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, an8855_core_of_match);

static struct mdio_driver an8855_core_driver = {
	.probe = an8855_core_probe,
	.mdiodrv.driver = {
		.name = "an8855",
		.of_match_table = an8855_core_of_match,
	},
};
mdio_module_driver(an8855_core_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Driver for Airoha AN8855 MFD");
MODULE_LICENSE("GPL");
