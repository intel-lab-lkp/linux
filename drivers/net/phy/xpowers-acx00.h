/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DRIVERS_NET_PHY_XPOWERS_ACX00_H
#define __DRIVERS_NET_PHY_XPOWERS_ACX00_H

#include <linux/phy.h>

struct acx00_ephy_control {
	int (*power_on)(struct acx00_ephy_control *control,
			unsigned int phy_addr);
	int (*power_off)(struct acx00_ephy_control *control);
	int (*set_interface)(struct acx00_ephy_control *control,
			     phy_interface_t interface);
};

#endif
