// SPDX-License-Identifier: GPL-2.0
// Broadcom BCM84881 NBASE-T PHY driver, as found on a SFP+ module.
// Copyright (C) 2019 Russell King, Deep Blue Solutions Ltd.
//
// Like the Marvell 88x3310, the Broadcom 84881 changes its host-side
// interface according to the operating speed between 10GBASE-R,
// 2500BASE-X and SGMII (but unlike the 88x3310, without the control
// word).
//
// This driver only supports those aspects of the PHY that I'm able to
// observe and test with the SFP+ module, which is an incomplete subset
// of what this PHY is able to support. For example, I only assume it
// supports a single lane Serdes connection, but it may be that the PHY
// is able to support more than that.
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/phy.h>

enum {
	MDIO_AN_C22 = 0xffe0,
};

/* BCM84891/92 vendor registers */
#define BCM8489X_LED_GATE		0xa82f	/* Dev1: gates amber only */
#define BCM8489X_LED_COLOR		0xa83b	/* Dev1: color select */

static int bcm84881_wait_init(struct phy_device *phydev)
{
	int val;

	return phy_read_mmd_poll_timeout(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1,
					 val, !(val & MDIO_CTRL1_RESET),
					 100000, 2000000, false);
}

static bool bcm84881_is_bcm8489x(struct phy_device *phydev)
{
	/* For C45 PHYs, phydev->phy_id is 0; match via the driver entry's
	 * declared ID, which is what phy_bus_match() used to bind us.
	 */
	u32 id = phydev->drv->phy_id;

	return (id & 0xfffffff0) == 0x35905080 ||
	       (id & 0xfffffff0) == 0x359050a0;
}

static void bcm84881_fill_possible_interfaces(struct phy_device *phydev)
{
	unsigned long *possible = phydev->possible_interfaces;

	__set_bit(PHY_INTERFACE_MODE_SGMII, possible);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, possible);
	__set_bit(PHY_INTERFACE_MODE_10GBASER, possible);
}

static int bcm84881_config_init(struct phy_device *phydev)
{
	bcm84881_fill_possible_interfaces(phydev);

	if (bcm84881_is_bcm8489x(phydev)) {
		__set_bit(PHY_INTERFACE_MODE_USXGMII,
			  phydev->possible_interfaces);
		/* Bootloader may have left the speed LED on, and
		 * link_change_notify won't fire for ports with no cable.
		 * Clear both color bits (green ignores the gate register).
		 */
		phy_clear_bits_mmd(phydev, MDIO_MMD_PMAPMD,
				   BCM8489X_LED_COLOR, 0x0012);
	}

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_USXGMII:
		break;
	default:
		return -ENODEV;
	}

	return 0;
}

static int bcm84881_probe(struct phy_device *phydev)
{
	/* This driver requires PMAPMD and AN blocks */
	const u32 mmd_mask = MDIO_DEVS_PMAPMD | MDIO_DEVS_AN;

	if (!phydev->is_c45 ||
	    (phydev->c45_ids.devices_in_package & mmd_mask) != mmd_mask)
		return -ENODEV;

	return 0;
}

static int bcm84881_get_features(struct phy_device *phydev)
{
	int ret;

	ret = genphy_c45_pma_read_abilities(phydev);
	if (ret)
		return ret;

	/* Although the PHY sets bit 1.11.8, it does not support 10M modes */
	linkmode_clear_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT,
			   phydev->supported);
	linkmode_clear_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT,
			   phydev->supported);

	return 0;
}

static int bcm84881_config_aneg(struct phy_device *phydev)
{
	bool changed = false;
	u32 adv;
	int ret;

	/* Wait for the PHY to finish initialising, otherwise our
	 * advertisement may be overwritten.
	 */
	ret = bcm84881_wait_init(phydev);
	if (ret)
		return ret;

	/* BCM84891/92 firmware has been observed setting MDIO_CTRL1_LPOWER
	 * autonomously (e.g. during SerDes reconfiguration). Clear it so AN
	 * can proceed.
	 */
	if (bcm84881_is_bcm8489x(phydev)) {
		ret = phy_clear_bits_mmd(phydev, MDIO_MMD_PMAPMD, MDIO_CTRL1,
					 MDIO_CTRL1_LPOWER);
		if (ret < 0)
			return ret;
	}

	/* We don't support manual MDI control */
	phydev->mdix_ctrl = ETH_TP_MDI_AUTO;

	/* disabled autoneg doesn't seem to work with this PHY */
	if (phydev->autoneg == AUTONEG_DISABLE)
		return -EINVAL;

	ret = genphy_c45_an_config_aneg(phydev);
	if (ret < 0)
		return ret;
	if (ret > 0)
		changed = true;

	adv = linkmode_adv_to_mii_ctrl1000_t(phydev->advertising);
	ret = phy_modify_mmd_changed(phydev, MDIO_MMD_AN,
				     MDIO_AN_C22 + MII_CTRL1000,
				     ADVERTISE_1000FULL | ADVERTISE_1000HALF,
				     adv);
	if (ret < 0)
		return ret;
	if (ret > 0)
		changed = true;

	return genphy_c45_check_and_restart_aneg(phydev, changed);
}

static int bcm84881_aneg_done(struct phy_device *phydev)
{
	int bmsr, val;

	val = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	if (val < 0)
		return val;

	bmsr = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_C22 + MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	return !!(val & MDIO_AN_STAT1_COMPLETE) &&
	       !!(bmsr & BMSR_ANEGCOMPLETE);
}

static int bcm84881_read_status(struct phy_device *phydev)
{
	unsigned int mode;
	int bmsr, val;

	val = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_CTRL1);
	if (val < 0)
		return val;

	if (val & MDIO_AN_CTRL1_RESTART) {
		phydev->link = 0;
		return 0;
	}

	val = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_STAT1);
	if (val < 0)
		return val;

	bmsr = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_C22 + MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	phydev->autoneg_complete = !!(val & MDIO_AN_STAT1_COMPLETE) &&
				   !!(bmsr & BMSR_ANEGCOMPLETE);
	phydev->link = !!(val & MDIO_STAT1_LSTATUS) &&
		       !!(bmsr & BMSR_LSTATUS);
	if (phydev->autoneg == AUTONEG_ENABLE && !phydev->autoneg_complete)
		phydev->link = false;

	linkmode_zero(phydev->lp_advertising);
	phydev->speed = SPEED_UNKNOWN;
	phydev->duplex = DUPLEX_UNKNOWN;
	phydev->pause = 0;
	phydev->asym_pause = 0;
	phydev->mdix = 0;

	if (!phydev->link)
		return 0;

	if (phydev->autoneg_complete) {
		val = genphy_c45_read_lpa(phydev);
		if (val < 0)
			return val;

		val = phy_read_mmd(phydev, MDIO_MMD_AN,
				   MDIO_AN_C22 + MII_STAT1000);
		if (val < 0)
			return val;

		mii_stat1000_mod_linkmode_lpa_t(phydev->lp_advertising, val);

		if (phydev->autoneg == AUTONEG_ENABLE)
			phy_resolve_aneg_linkmode(phydev);
	}

	if (phydev->autoneg == AUTONEG_DISABLE) {
		/* disabled autoneg doesn't seem to work, so force the link
		 * down.
		 */
		phydev->link = 0;
		return 0;
	}

	/* For BCM84891/92 in USXGMII mode, the host-side register 0x4011
	 * reports 10GBASER (the USXGMII SerDes line rate) regardless of the
	 * negotiated copper speed. Skip it; phy_resolve_aneg_linkmode()
	 * above already set the correct speed from standard AN resolution.
	 */
	if (bcm84881_is_bcm8489x(phydev) &&
	    phydev->interface == PHY_INTERFACE_MODE_USXGMII)
		return genphy_c45_read_mdix(phydev);

	/* Set the host link mode - we set the phy interface mode and
	 * the speed according to this register so that downshift works.
	 * We leave the duplex setting as per the resolution from the
	 * above.
	 */
	val = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x4011);
	mode = (val & 0x1e) >> 1;
	if (mode == 1 || mode == 2)
		phydev->interface = PHY_INTERFACE_MODE_SGMII;
	else if (mode == 3)
		phydev->interface = PHY_INTERFACE_MODE_10GBASER;
	else if (mode == 4)
		phydev->interface = PHY_INTERFACE_MODE_2500BASEX;
	switch (mode & 7) {
	case 1:
		phydev->speed = SPEED_100;
		break;
	case 2:
		phydev->speed = SPEED_1000;
		break;
	case 3:
		phydev->speed = SPEED_10000;
		break;
	case 4:
		phydev->speed = SPEED_2500;
		break;
	case 5:
		phydev->speed = SPEED_5000;
		break;
	}

	return genphy_c45_read_mdix(phydev);
}

/* BCM8489x speed LED control.
 * Dev1:a83b bit 1 (0x0002) = green, bit 4 (0x0010) = amber.
 * Dev1:a82f = 0x0020 gates amber on; green ignores the gate.
 * Writes are best-effort (LED is cosmetic; this callback is void).
 */
static void bcm84881_link_change_notify(struct phy_device *phydev)
{
	if (phydev->link) {
		/* 10G = green, else amber; matches vendor firmware */
		u16 color = (phydev->speed == SPEED_10000) ? 0x0002 : 0x0010;

		phy_write_mmd(phydev, MDIO_MMD_PMAPMD, BCM8489X_LED_GATE, 0x0020);
		phy_modify_mmd(phydev, MDIO_MMD_PMAPMD, BCM8489X_LED_COLOR,
			       0x0012, color);
	} else {
		phy_clear_bits_mmd(phydev, MDIO_MMD_PMAPMD,
				   BCM8489X_LED_COLOR, 0x0012);
	}
}

/* The Broadcom BCM84881 in the Methode DM7052 is unable to provide a SGMII
 * or 802.3z control word, so inband will not work.
 */
static unsigned int bcm84881_inband_caps(struct phy_device *phydev,
					 phy_interface_t interface)
{
	return LINK_INBAND_DISABLE;
}

static struct phy_driver bcm84881_drivers[] = {
	{
		.phy_id		= 0xae025150,
		.phy_id_mask	= 0xfffffff0,
		.name		= "Broadcom BCM84881",
		.inband_caps	= bcm84881_inband_caps,
		.config_init	= bcm84881_config_init,
		.probe		= bcm84881_probe,
		.get_features	= bcm84881_get_features,
		.config_aneg	= bcm84881_config_aneg,
		.aneg_done	= bcm84881_aneg_done,
		.read_status	= bcm84881_read_status,
	},
	{
		.phy_id		= 0x35905081,
		.phy_id_mask	= 0xfffffff0,
		.name		= "Broadcom BCM84891",
		.inband_caps	= bcm84881_inband_caps,
		.config_init	= bcm84881_config_init,
		.probe		= bcm84881_probe,
		.get_features	= bcm84881_get_features,
		.config_aneg	= bcm84881_config_aneg,
		.aneg_done	= bcm84881_aneg_done,
		.read_status	= bcm84881_read_status,
		.link_change_notify = bcm84881_link_change_notify,
	},
	{
		.phy_id		= 0x359050a1,
		.phy_id_mask	= 0xfffffff0,
		.name		= "Broadcom BCM84892",
		.inband_caps	= bcm84881_inband_caps,
		.config_init	= bcm84881_config_init,
		.probe		= bcm84881_probe,
		.get_features	= bcm84881_get_features,
		.config_aneg	= bcm84881_config_aneg,
		.aneg_done	= bcm84881_aneg_done,
		.read_status	= bcm84881_read_status,
		.link_change_notify = bcm84881_link_change_notify,
	},
};

module_phy_driver(bcm84881_drivers);

/* FIXME: module auto-loading for Clause 45 PHYs seems non-functional */
static const struct mdio_device_id __maybe_unused bcm84881_tbl[] = {
	{ 0xae025150, 0xfffffff0 },
	{ 0x35905081, 0xfffffff0 },
	{ 0x359050a1, 0xfffffff0 },
	{ },
};
MODULE_AUTHOR("Russell King");
MODULE_DESCRIPTION("Broadcom BCM84881/BCM84891/BCM84892 PHY driver");
MODULE_DEVICE_TABLE(mdio, bcm84881_tbl);
MODULE_LICENSE("GPL");
