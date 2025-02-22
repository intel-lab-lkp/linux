// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/ethtool.h>
#include <linux/linkmode.h>
#include <linux/phy.h>
#include <linux/phylink.h>

#include "phy-caps.h"

/**
 * linkmode_from_caps() - Convert capabilities to ethtool link modes
 * @linkmodes: ethtool linkmode mask (must be already initialised)
 * @caps: bitmask of MAC capabilities
 *
 * Set all possible pause, speed and duplex linkmodes in @linkmodes that are
 * supported by the @caps. @linkmodes must have been initialised previously.
 */

void linkmode_from_caps(unsigned long *linkmodes, unsigned long caps)
{
	if (caps & MAC_SYM_PAUSE)
		__set_bit(ETHTOOL_LINK_MODE_Pause_BIT, linkmodes);

	if (caps & MAC_ASYM_PAUSE)
		__set_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT, linkmodes);

	if (caps & MAC_10HD) {
		__set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10baseT1S_Half_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10baseT1S_P2MP_Half_BIT, linkmodes);
	}

	if (caps & MAC_10FD) {
		__set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10baseT1L_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10baseT1S_Full_BIT, linkmodes);
	}

	if (caps & MAC_100HD) {
		__set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100baseFX_Half_BIT, linkmodes);
	}

	if (caps & MAC_100FD) {
		__set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100baseT1_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100baseFX_Full_BIT, linkmodes);
	}

	if (caps & MAC_1000HD)
		__set_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, linkmodes);

	if (caps & MAC_1000FD) {
		__set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_1000baseKX_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_1000baseX_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_1000baseT1_Full_BIT, linkmodes);
	}

	if (caps & MAC_2500FD) {
		__set_bit(ETHTOOL_LINK_MODE_2500baseT_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_2500baseX_Full_BIT, linkmodes);
	}

	if (caps & MAC_5000FD)
		__set_bit(ETHTOOL_LINK_MODE_5000baseT_Full_BIT, linkmodes);

	if (caps & MAC_10000FD) {
		__set_bit(ETHTOOL_LINK_MODE_10000baseT_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseKX4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseKR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseR_FEC_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseCR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseSR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseLR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseLRM_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_10000baseER_Full_BIT, linkmodes);
	}

	if (caps & MAC_25000FD) {
		__set_bit(ETHTOOL_LINK_MODE_25000baseCR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_25000baseKR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_25000baseSR_Full_BIT, linkmodes);
	}

	if (caps & MAC_40000FD) {
		__set_bit(ETHTOOL_LINK_MODE_40000baseKR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_40000baseCR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_40000baseSR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_40000baseLR4_Full_BIT, linkmodes);
	}

	if (caps & MAC_50000FD) {
		__set_bit(ETHTOOL_LINK_MODE_50000baseCR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseKR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseSR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseKR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseSR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseCR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseLR_ER_FR_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_50000baseDR_Full_BIT, linkmodes);
	}

	if (caps & MAC_56000FD) {
		__set_bit(ETHTOOL_LINK_MODE_56000baseKR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_56000baseCR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_56000baseSR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_56000baseLR4_Full_BIT, linkmodes);
	}

	if (caps & MAC_100000FD) {
		__set_bit(ETHTOOL_LINK_MODE_100000baseKR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseSR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseCR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseLR4_ER4_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseKR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseSR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseCR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseLR2_ER2_FR2_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseDR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseKR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseSR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseLR_ER_FR_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseCR_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_100000baseDR_Full_BIT, linkmodes);
	}

	if (caps & MAC_200000FD) {
		__set_bit(ETHTOOL_LINK_MODE_200000baseKR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseSR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseLR4_ER4_FR4_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseDR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseCR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseKR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseSR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseLR2_ER2_FR2_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseDR2_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_200000baseCR2_Full_BIT, linkmodes);
	}

	if (caps & MAC_400000FD) {
		__set_bit(ETHTOOL_LINK_MODE_400000baseKR8_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseSR8_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseLR8_ER8_FR8_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseDR8_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseCR8_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseKR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseSR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseLR4_ER4_FR4_Full_BIT,
			  linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseDR4_Full_BIT, linkmodes);
		__set_bit(ETHTOOL_LINK_MODE_400000baseCR4_Full_BIT, linkmodes);
	}
}
EXPORT_SYMBOL_GPL(linkmode_from_caps);

/**
 * phy_interface_caps() - Retrieve the speed/duplex capablities of an interface
 * @interface: phy_interface_t to get the capabilities from
 *
 * Returns: A bitmask of MAC_* capablities for a given interface.
 */
unsigned long phy_interface_caps(phy_interface_t interface)
{
	unsigned long caps = MAC_SYM_PAUSE | MAC_ASYM_PAUSE;

	switch (interface) {
	case PHY_INTERFACE_MODE_USXGMII:
		caps |= MAC_10000FD | MAC_5000FD;
		fallthrough;

	case PHY_INTERFACE_MODE_10G_QXGMII:
		caps |= MAC_2500FD;
		fallthrough;

	case PHY_INTERFACE_MODE_RGMII_TXID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_PSGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_QUSGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_GMII:
		caps |= MAC_1000HD | MAC_1000FD;
		fallthrough;

	case PHY_INTERFACE_MODE_REVRMII:
	case PHY_INTERFACE_MODE_RMII:
	case PHY_INTERFACE_MODE_SMII:
	case PHY_INTERFACE_MODE_REVMII:
	case PHY_INTERFACE_MODE_MII:
		caps |= MAC_10HD | MAC_10FD;
		fallthrough;

	case PHY_INTERFACE_MODE_100BASEX:
		caps |= MAC_100HD | MAC_100FD;
		break;

	case PHY_INTERFACE_MODE_TBI:
	case PHY_INTERFACE_MODE_MOCA:
	case PHY_INTERFACE_MODE_RTBI:
	case PHY_INTERFACE_MODE_1000BASEX:
		caps |= MAC_1000HD;
		fallthrough;
	case PHY_INTERFACE_MODE_1000BASEKX:
	case PHY_INTERFACE_MODE_TRGMII:
		caps |= MAC_1000FD;
		break;

	case PHY_INTERFACE_MODE_2500BASEX:
		caps |= MAC_2500FD;
		break;

	case PHY_INTERFACE_MODE_5GBASER:
		caps |= MAC_5000FD;
		break;

	case PHY_INTERFACE_MODE_XGMII:
	case PHY_INTERFACE_MODE_RXAUI:
	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10GKR:
		caps |= MAC_10000FD;
		break;

	case PHY_INTERFACE_MODE_25GBASER:
		caps |= MAC_25000FD;
		break;

	case PHY_INTERFACE_MODE_XLGMII:
		caps |= MAC_40000FD;
		break;

	case PHY_INTERFACE_MODE_INTERNAL:
		caps |= ~0;
		break;

	case PHY_INTERFACE_MODE_NA:
	case PHY_INTERFACE_MODE_MAX:
		break;
	}

	return caps;
}
EXPORT_SYMBOL_GPL(phy_interface_caps);

/**
 * phy_interface_max_speed() - get the maximum speed of a phy interface
 * @interface: phy interface mode defined by &typedef phy_interface_t
 *
 * Determine the maximum speed of a phy interface. This is intended to help
 * determine the correct speed to pass to the MAC when the phy is performing
 * rate matching.
 *
 * Return: The maximum speed of @interface
 */
int phy_interface_max_speed(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_100BASEX:
	case PHY_INTERFACE_MODE_REVRMII:
	case PHY_INTERFACE_MODE_RMII:
	case PHY_INTERFACE_MODE_SMII:
	case PHY_INTERFACE_MODE_REVMII:
	case PHY_INTERFACE_MODE_MII:
		return SPEED_100;

	case PHY_INTERFACE_MODE_TBI:
	case PHY_INTERFACE_MODE_MOCA:
	case PHY_INTERFACE_MODE_RTBI:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_1000BASEKX:
	case PHY_INTERFACE_MODE_TRGMII:
	case PHY_INTERFACE_MODE_RGMII_TXID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_PSGMII:
	case PHY_INTERFACE_MODE_QSGMII:
	case PHY_INTERFACE_MODE_QUSGMII:
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_GMII:
		return SPEED_1000;

	case PHY_INTERFACE_MODE_2500BASEX:
	case PHY_INTERFACE_MODE_10G_QXGMII:
		return SPEED_2500;

	case PHY_INTERFACE_MODE_5GBASER:
		return SPEED_5000;

	case PHY_INTERFACE_MODE_XGMII:
	case PHY_INTERFACE_MODE_RXAUI:
	case PHY_INTERFACE_MODE_XAUI:
	case PHY_INTERFACE_MODE_10GBASER:
	case PHY_INTERFACE_MODE_10GKR:
	case PHY_INTERFACE_MODE_USXGMII:
		return SPEED_10000;

	case PHY_INTERFACE_MODE_25GBASER:
		return SPEED_25000;

	case PHY_INTERFACE_MODE_XLGMII:
		return SPEED_40000;

	case PHY_INTERFACE_MODE_INTERNAL:
	case PHY_INTERFACE_MODE_NA:
	case PHY_INTERFACE_MODE_MAX:
		/* No idea! Garbage in, unknown out */
		return SPEED_UNKNOWN;
	}

	/* If we get here, someone forgot to add an interface mode above */
	WARN_ON_ONCE(1);
	return SPEED_UNKNOWN;
}
EXPORT_SYMBOL_GPL(phy_interface_max_speed);
