// SPDX-License-Identifier: GPL-2.0+

#include <linux/phy.h>

#include "phylib-internal.h"

/**
 * phy_register_address_0_fixups - Register fixups for disabling MDIO
 * broadcast address 0
 *
 * Some vendors interpret MDIO address 0 as a broadcast address and so their
 * PHYs initially respond both at address 0 as well as at their normal
 * address. This is problematic:
 *  - Linux may create two struct phy_device for a single device.
 *  - MDIO address collision may occur if there is another PHY
 *    legitimately listening on address 0.
 *
 * The broadcast address can often be disabled through some internal
 * PHY register. These fixups are designed to do just that --
 * they shall stop these PHYs from responding at address 0 before
 * the MDIO address 0 is scanned for devices.
 */
int phy_register_address_0_fixups(void)
{
	return 0;
}
