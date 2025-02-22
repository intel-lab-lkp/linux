// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/ethtool.h>
#include <linux/linkmode.h>
#include <linux/phy.h>
#include <linux/phylink.h>

#include "phy-caps.h"

enum {
	LINK_CAPA_10HD = 0,
	LINK_CAPA_10FD,
	LINK_CAPA_100HD,
	LINK_CAPA_100FD,
	LINK_CAPA_1000HD,
	LINK_CAPA_1000FD,
	LINK_CAPA_2500FD,
	LINK_CAPA_5000FD,
	LINK_CAPA_10000FD,
	LINK_CAPA_20000FD,
	LINK_CAPA_25000FD,
	LINK_CAPA_40000FD,
	LINK_CAPA_50000FD,
	LINK_CAPA_56000FD,
	LINK_CAPA_100000FD,
	LINK_CAPA_200000FD,
	LINK_CAPA_400000FD,
	LINK_CAPA_800000FD,

	__LINK_CAPA_LAST = LINK_CAPA_800000FD,
	__LINK_CAPA_MAX,
};

static struct link_capabilities link_caps[__LINK_CAPA_MAX] __ro_after_init = {
	{ SPEED_10, DUPLEX_HALF, {0} }, /* LINK_CAPA_10HD */
	{ SPEED_10, DUPLEX_FULL, {0} }, /* LINK_CAPA_10FD */
	{ SPEED_100, DUPLEX_HALF, {0} }, /* LINK_CAPA_100HD */
	{ SPEED_100, DUPLEX_FULL, {0} }, /* LINK_CAPA_100FD */
	{ SPEED_1000, DUPLEX_HALF, {0} }, /* LINK_CAPA_1000HD */
	{ SPEED_1000, DUPLEX_FULL, {0} }, /* LINK_CAPA_1000FD */
	{ SPEED_2500, DUPLEX_FULL, {0} }, /* LINK_CAPA_2500FD */
	{ SPEED_5000, DUPLEX_FULL, {0} }, /* LINK_CAPA_5000FD */
	{ SPEED_10000, DUPLEX_FULL, {0} }, /* LINK_CAPA_10000FD */
	{ SPEED_20000, DUPLEX_FULL, {0} }, /* LINK_CAPA_20000FD */
	{ SPEED_25000, DUPLEX_FULL, {0} }, /* LINK_CAPA_25000FD */
	{ SPEED_40000, DUPLEX_FULL, {0} }, /* LINK_CAPA_40000FD */
	{ SPEED_50000, DUPLEX_FULL, {0} }, /* LINK_CAPA_50000FD */
	{ SPEED_56000, DUPLEX_FULL, {0} }, /* LINK_CAPA_56000FD */
	{ SPEED_100000, DUPLEX_FULL, {0} }, /* LINK_CAPA_100000FD */
	{ SPEED_200000, DUPLEX_FULL, {0} }, /* LINK_CAPA_200000FD */
	{ SPEED_400000, DUPLEX_FULL, {0} }, /* LINK_CAPA_400000FD */
	{ SPEED_800000, DUPLEX_FULL, {0} }, /* LINK_CAPA_800000FD */
};

static int speed_duplex_to_capa(int speed, unsigned int duplex)
{
	if (duplex == DUPLEX_UNKNOWN ||
	    (speed > SPEED_1000 && duplex != DUPLEX_FULL))
		return -EINVAL;

	switch (speed) {
	case SPEED_10: return duplex == DUPLEX_FULL ?
			      LINK_CAPA_10FD : LINK_CAPA_10HD;
	case SPEED_100: return duplex == DUPLEX_FULL ?
			       LINK_CAPA_100FD : LINK_CAPA_100HD;
	case SPEED_1000: return duplex == DUPLEX_FULL ?
				LINK_CAPA_1000FD : LINK_CAPA_1000HD;
	case SPEED_2500: return LINK_CAPA_2500FD;
	case SPEED_5000: return LINK_CAPA_5000FD;
	case SPEED_10000: return LINK_CAPA_10000FD;
	case SPEED_20000: return LINK_CAPA_20000FD;
	case SPEED_25000: return LINK_CAPA_25000FD;
	case SPEED_40000: return LINK_CAPA_40000FD;
	case SPEED_50000: return LINK_CAPA_50000FD;
	case SPEED_56000: return LINK_CAPA_56000FD;
	case SPEED_100000: return LINK_CAPA_100000FD;
	case SPEED_200000: return LINK_CAPA_200000FD;
	case SPEED_400000: return LINK_CAPA_400000FD;
	case SPEED_800000: return LINK_CAPA_800000FD;
	}

	return -EINVAL;
}

/**
 * phy_caps_init() - Initializes the link_caps array from the link_mode_params.
 */
void phy_caps_init(void)
{
	const struct link_mode_info *linkmode;
	int i, capa;

	/* Fill the caps array from net/ethtool/common.c */
	for (i = 0; i < __ETHTOOL_LINK_MODE_MASK_NBITS; i++) {
		linkmode = &link_mode_params[i];
		capa = speed_duplex_to_capa(linkmode->speed, linkmode->duplex);

		if (capa < 0)
			continue;

		__set_bit(i, link_caps[capa].linkmodes);
	}
}

/**
 * phy_caps_speeds() - Fill an array of supported SPEED_* values for given modes
 * @speeds: Output array to store the speeds list into
 * @size: Size of the output array
 * @linkmodes: Linkmodes to get the speeds from
 *
 * Fills the speeds array with all possible speeds that can be achieved with
 * the specified linkmodes.
 *
 * Returns: The number of speeds filled into the array. If the input array isn't
 *	    big enough to store all speeds, fill it as much as possible.
 */
size_t phy_caps_speeds(unsigned int *speeds, size_t size,
		       unsigned long *linkmodes)
{
	size_t count;
	int capa;

	for (capa = 0, count = 0; capa < __LINK_CAPA_MAX && count < size; capa++) {
		if (linkmode_intersects(link_caps[capa].linkmodes, linkmodes) &&
		    (count == 0 || speeds[count - 1] != link_caps[capa].speed))
			speeds[count++] = link_caps[capa].speed;
	}

	return count;
}

/**
 * phy_caps_lookup_by_linkmode() - Lookup the fastest matching link_capabilities
 * @linkmodes: Linkmodes to match against
 *
 * Returns: The highest-speed link_capabilities that intersects the given
 *	    linkmodes. In case several DUPLEX_ options exist at that speed,
 *	    DUPLEX_FULL is matched first. NULL is returned if no match.
 */
const struct link_capabilities *
phy_caps_lookup_by_linkmode(const unsigned long *linkmodes)
{
	int capa;

	for (capa = __LINK_CAPA_LAST; capa >= 0; capa--)
		if (linkmode_intersects(link_caps[capa].linkmodes, linkmodes))
			return &link_caps[capa];

	return NULL;
}

/**
 * phy_caps_lookup_by_linkmode_rev() - Lookup the slowest matching link_capabilities
 * @linkmodes: Linkmodes to match against
 * @fdx_only: Full duplex match only when set
 *
 * Returns: The lowest-speed link_capabilities that intersects the given
 *	    linkmodes. When set, fdx_only will ignore half-duplex matches.
 *	    NULL is returned if no match.
 */
const struct link_capabilities *
phy_caps_lookup_by_linkmode_rev(const unsigned long *linkmodes, bool fdx_only)
{
	int capa;

	for (capa = 0; capa < __LINK_CAPA_MAX; capa++) {
		if (fdx_only && link_caps[capa].duplex != DUPLEX_FULL)
			continue;

		if (linkmode_intersects(link_caps[capa].linkmodes, linkmodes))
			return &link_caps[capa];
	}

	return NULL;
}

/**
 * phy_caps_linkmode_max_speed() - Clamp a linkmodes set to a max speed
 * @max_speed: Speed limit for the linkmode set
 * @linkmodes: Linkmodes to limit
 */
void phy_caps_linkmode_max_speed(u32 max_speed, unsigned long *linkmodes)
{
	int capa;

	for (capa = __LINK_CAPA_LAST; capa >= 0; capa--)
		if (link_caps[capa].speed > max_speed)
			linkmode_andnot(linkmodes, linkmodes, link_caps[capa].linkmodes);
		else
			break;
}

/**
 * phy_caps_valid() - Validate a linkmodes set agains given speed and duplex
 */
bool phy_caps_valid(int speed, int duplex, const unsigned long *linkmodes)
{
	int capa = speed_duplex_to_capa(speed, duplex);

	if (capa < 0)
		return false;

	return linkmode_intersects(link_caps[capa].linkmodes, linkmodes);
}

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

	if (caps & MAC_10HD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_10HD].linkmodes);

	if (caps & MAC_10FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_10FD].linkmodes);

	if (caps & MAC_100HD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_100HD].linkmodes);

	if (caps & MAC_100FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_100FD].linkmodes);

	if (caps & MAC_1000HD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_1000HD].linkmodes);

	if (caps & MAC_1000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_1000FD].linkmodes);

	if (caps & MAC_2500FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_2500FD].linkmodes);

	if (caps & MAC_5000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_5000FD].linkmodes);

	if (caps & MAC_10000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_10000FD].linkmodes);

	if (caps & MAC_25000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_25000FD].linkmodes);

	if (caps & MAC_40000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_40000FD].linkmodes);

	if (caps & MAC_50000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_50000FD].linkmodes);

	if (caps & MAC_56000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_56000FD].linkmodes);

	if (caps & MAC_100000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_100000FD].linkmodes);

	if (caps & MAC_200000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_200000FD].linkmodes);

	if (caps & MAC_400000FD)
		linkmode_or(linkmodes, linkmodes,
			    link_caps[LINK_CAPA_400000FD].linkmodes);
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
