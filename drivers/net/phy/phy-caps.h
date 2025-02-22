/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * link caps internal header, for link modes <-> capabilities <-> interfaces
 * conversions.
 */

#ifndef __PHY_CAPS_H
#define __PHY_CAPS_H

#include <linux/ethtool.h>

struct link_capabilities {
	int speed;
	unsigned int duplex;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(linkmodes);
};

void phy_caps_init(void);
void linkmode_from_caps(unsigned long *linkmode, unsigned long caps);

unsigned long phy_interface_caps(phy_interface_t interface);
int phy_interface_max_speed(phy_interface_t interface);

size_t phy_caps_speeds(unsigned int *speeds, size_t size,
		       unsigned long *linkmodes);

#endif /* __PHY_CAPS_H */
