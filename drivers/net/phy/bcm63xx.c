// SPDX-License-Identifier: GPL-2.0+
/*
 *	Driver for Broadcom 63xx SOCs integrated PHYs
 */
#include "bcm-phy-lib.h"
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define MII_BCM63XX_IR		0x1a	/* interrupt register */
#define MII_BCM63XX_IR_EN	0x4000	/* global interrupt enable */
#define MII_BCM63XX_IR_DUPLEX	0x0800	/* duplex changed */
#define MII_BCM63XX_IR_SPEED	0x0400	/* speed changed */
#define MII_BCM63XX_IR_LINK	0x0200	/* link changed */
#define MII_BCM63XX_IR_GMASK	0x0100	/* global interrupt mask */

#define PHY_ID_BCM63268_GPHY	0x03625f50

#define GPHY_CTRL_OFFSET	0x54
#define GPHY_CTRL_IDDQ_BIAS	BIT(0)
#define GPHY_CTRL_LOW_PWR	BIT(3)

MODULE_DESCRIPTION("Broadcom 63xx internal PHY driver");
MODULE_AUTHOR("Maxime Bizon <mbizon@freebox.fr>");
MODULE_LICENSE("GPL");

struct bcm_gphy_priv {
	struct regmap *gpio_ctrl;
};

static int bcm63xx_config_intr(struct phy_device *phydev)
{
	int reg, err;

	reg = phy_read(phydev, MII_BCM63XX_IR);
	if (reg < 0)
		return reg;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = bcm_phy_ack_intr(phydev);
		if (err)
			return err;

		reg &= ~MII_BCM63XX_IR_GMASK;
		err = phy_write(phydev, MII_BCM63XX_IR, reg);
	} else {
		reg |= MII_BCM63XX_IR_GMASK;
		err = phy_write(phydev, MII_BCM63XX_IR, reg);
		if (err)
			return err;

		err = bcm_phy_ack_intr(phydev);
	}

	return err;
}

static int bcm63xx_config_init(struct phy_device *phydev)
{
	int reg, err;

	/* ASYM_PAUSE bit is marked RO in datasheet, so don't cheat */
	linkmode_set_bit(ETHTOOL_LINK_MODE_Pause_BIT, phydev->supported);

	reg = phy_read(phydev, MII_BCM63XX_IR);
	if (reg < 0)
		return reg;

	/* Mask interrupts globally.  */
	reg |= MII_BCM63XX_IR_GMASK;
	err = phy_write(phydev, MII_BCM63XX_IR, reg);
	if (err < 0)
		return err;

	/* Unmask events we are interested in  */
	reg = ~(MII_BCM63XX_IR_DUPLEX |
		MII_BCM63XX_IR_SPEED |
		MII_BCM63XX_IR_LINK) |
		MII_BCM63XX_IR_EN;
	return phy_write(phydev, MII_BCM63XX_IR, reg);
}

static int bcm63268_gphy_set(struct phy_device *phydev, bool enable)
{
	struct bcm_gphy_priv *priv = phydev->priv;
	u32 pwr_bits;
	int ret;

	pwr_bits = GPHY_CTRL_IDDQ_BIAS | GPHY_CTRL_LOW_PWR;

	if (enable)
		ret = regmap_update_bits(priv->gpio_ctrl, GPHY_CTRL_OFFSET, pwr_bits, 0);
	else
		ret = regmap_update_bits(priv->gpio_ctrl, GPHY_CTRL_OFFSET, pwr_bits, pwr_bits);

	return ret;
}

static int bcm63268_gphy_resume(struct phy_device *phydev)
{
	int ret;

	ret = bcm63268_gphy_set(phydev, true);
	if (ret)
		return ret;

	return genphy_resume(phydev);
}

static int bcm63268_gphy_suspend(struct phy_device *phydev)
{
	int ret;

	ret = genphy_suspend(phydev);
	if (ret)
		return ret;

	return bcm63268_gphy_set(phydev, false);
}

static int bcm63268_gphy_probe(struct phy_device *phydev)
{
	struct mdio_device *mdio = &phydev->mdio;
	struct device *dev = &mdio->dev;
	struct reset_control *reset;
	struct bcm_gphy_priv *priv;
	struct regmap *regmap;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "brcm,gpio-ctrl");
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	priv->gpio_ctrl = regmap;

	reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return PTR_ERR(reset);

	return reset_control_reset(reset);
}

static struct phy_driver bcm63xx_driver[] = {
{
	.phy_id		= 0x00406000,
	.phy_id_mask	= 0xfffffc00,
	.name		= "Broadcom BCM63XX (1)",
	/* PHY_BASIC_FEATURES */
	.flags		= PHY_IS_INTERNAL,
	.config_init	= bcm63xx_config_init,
	.config_intr	= bcm63xx_config_intr,
	.handle_interrupt = bcm_phy_handle_interrupt,
}, {
	/* same phy as above, with just a different OUI */
	.phy_id		= 0x002bdc00,
	.phy_id_mask	= 0xfffffc00,
	.name		= "Broadcom BCM63XX (2)",
	/* PHY_BASIC_FEATURES */
	.flags		= PHY_IS_INTERNAL,
	.config_init	= bcm63xx_config_init,
	.config_intr	= bcm63xx_config_intr,
	.handle_interrupt = bcm_phy_handle_interrupt,
}, {
	.phy_id         = PHY_ID_BCM63268_GPHY,
	.phy_id_mask    = 0xfffffff0,
	.name           = "Broadcom BCM63268 GPHY",
	/* PHY_BASIC_FEATURES */
	.flags          = PHY_IS_INTERNAL,
	.probe          = bcm63268_gphy_probe,
	.resume			= bcm63268_gphy_resume,
	.suspend		= bcm63268_gphy_suspend,
} };

module_phy_driver(bcm63xx_driver);

static const struct mdio_device_id __maybe_unused bcm63xx_tbl[] = {
	{ 0x00406000, 0xfffffc00 },
	{ 0x002bdc00, 0xfffffc00 },
	{ PHY_ID_MATCH_EXACT(PHY_ID_BCM63268_GPHY) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, bcm63xx_tbl);
