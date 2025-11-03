// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/ethtool.h>
#include <linux/kernel.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/pcs/pcs-xpcs.h>
#include <linux/phylink.h>

MODULE_DESCRIPTION("Meta Platforms FBNIC PHY driver");
MODULE_LICENSE("GPL");

static int fbnic_phy_match_phy_device(struct phy_device *phydev,
				      const struct phy_driver *phydrv)
{
	u32 *device_ids = phydev->c45_ids.device_ids;

	return device_ids[MDIO_MMD_PMAPMD] == MP_FBNIC_XPCS_PMA_100G_ID &&
	       device_ids[MDIO_MMD_PCS] == DW_XPCS_ID;
}

static int fbnic_phy_get_features(struct phy_device *phydev)
{
	phylink_set(phydev->supported, 100000baseCR2_Full);
	phylink_set(phydev->supported, 50000baseCR_Full);
	phylink_set(phydev->supported, 50000baseCR2_Full);
	phylink_set(phydev->supported, 25000baseCR_Full);

	return 0;
}

static struct phy_driver fbnic_phy_driver[] = {
{
	.phy_id			= MP_FBNIC_XPCS_PMA_100G_ID,
	.phy_id_mask		= 0xffffffff,
	.name			= "Meta Platforms FBNIC PHY Driver",
	.match_phy_device	= fbnic_phy_match_phy_device,
	.get_features		= fbnic_phy_get_features,
	.read_status		= genphy_c45_read_status,
	.config_aneg		= gen10g_config_aneg,
},
};

module_phy_driver(fbnic_phy_driver);

static const struct mdio_device_id __maybe_unused fbnic_phy_tbl[] = {
	{ MP_FBNIC_XPCS_PMA_100G_ID, 0xffffffff },
	{ }
};

MODULE_DEVICE_TABLE(mdio, fbnic_phy_tbl);
