// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Analog Devices, Inc. ADIN1140 10BASE-T1S PHY
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

#define ADIN1140_PHY_ID		0x0283be00

#define ADIN1140_PCS_CTRL		0x08f3
#define ADIN1140_PCS_CTRL_LOOPBACK	BIT(14)

static int adin1140_phy_read_mmd(struct phy_device *phydev, int devnum,
				 u16 regnum)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int addr = phydev->mdio.addr;

	return __mdiobus_c45_read(bus, addr, devnum, regnum);
}

static int adin1140_phy_write_mmd(struct phy_device *phydev, int devnum,
				  u16 regnum, u16 val)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int addr = phydev->mdio.addr;

	return __mdiobus_c45_write(bus, addr, devnum, regnum, val);
}

static int adin1140_config_init(struct phy_device *phydev)
{
	/* The link status of the PHY doesn't need to be polled, because
	 * the device doesn't implement AN and there is no other mechanism
	 * to report the link state.
	 */
	phydev->irq = PHY_MAC_INTERRUPT;

	return 0;
}

static int adin1140_config_aneg(struct phy_device *phydev)
{
	/* phylib tries to clear BIT(12) in MDIO_CTRL1, since AN is disabled.
	 * However, on the ADIN1140, that field is non-standard, being used
	 * to control the reset status of the PHY (thus it needs to remain set).
	 */
	return 0;
}

static int adin1140_loopback(struct phy_device *phydev, bool enable, int speed)
{
	if (enable && speed)
		return -EOPNOTSUPP;

	return phy_modify_mmd(phydev, MDIO_MMD_PCS, ADIN1140_PCS_CTRL,
			      ADIN1140_PCS_CTRL_LOOPBACK,
			      enable ? ADIN1140_PCS_CTRL_LOOPBACK : 0);
}

static int adin1140_read_status(struct phy_device *phydev)
{
	phydev->link = 1;
	phydev->duplex = DUPLEX_HALF;
	phydev->speed = SPEED_10;
	phydev->autoneg = AUTONEG_DISABLE;

	return 0;
}

static struct phy_driver adin1140_driver[] = {
	{
		PHY_ID_MATCH_EXACT(ADIN1140_PHY_ID),
		.name = "ADIN1140",
		.features = PHY_BASIC_T1S_P2MP_FEATURES,
		.read_status = adin1140_read_status,
		.config_init = adin1140_config_init,
		.config_aneg = adin1140_config_aneg,
		.set_loopback = adin1140_loopback,
		.read_mmd = adin1140_phy_read_mmd,
		.write_mmd = adin1140_phy_write_mmd,
		.get_plca_cfg = genphy_c45_plca_get_cfg,
		.set_plca_cfg = genphy_c45_plca_set_cfg,
		.get_plca_status = genphy_c45_plca_get_status,
	},
};
module_phy_driver(adin1140_driver);

static const struct mdio_device_id __maybe_unused adin1140_tbl[] = {
	{ PHY_ID_MATCH_EXACT(ADIN1140_PHY_ID) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, adin1140_tbl);

MODULE_DESCRIPTION("Analog Devices, Inc. ADIN1140 10BASE-T1S PHY");
MODULE_AUTHOR("Ciprian Regus <ciprian.regus@analog.com>");
MODULE_LICENSE("GPL");
