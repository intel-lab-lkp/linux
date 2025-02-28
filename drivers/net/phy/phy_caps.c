// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/ethtool.h>
#include <linux/linkmode.h>
#include <linux/phy.h>

#include "phy-caps.h"

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
 * @speed: input speed to validate
 * @duplex: input duplex to validate. Passing DUPLEX_UNKNOWN is always not valid
 * @linkmodes: The linkmodes to validate
 *
 * Returns: True if at least one of the linkmodes in @linkmodes can function at
 *          the given speed and duplex, false otherwise.
 */
bool phy_caps_valid(int speed, int duplex, const unsigned long *linkmodes)
{
	int capa = speed_duplex_to_capa(speed, duplex);

	if (capa < 0)
		return false;

	return linkmode_intersects(link_caps[capa].linkmodes, linkmodes);
}
