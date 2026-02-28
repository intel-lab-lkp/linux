// SPDX-License-Identifier: GPL-2.0+

#include <linux/phy.h>

#include "phylib-internal.h"

#define PHY_ID_YT8821				0x4f51ea19
#define YTPHY_PAGE_SELECT			0x1E
#define YTPHY_PAGE_DATA				0x1F
#define YTPHY_MDIO_ADDRESS_CONTROL_REG		0xA005
#define YTPHY_MACR_EN_PHY_ADDR_0		BIT(6)

/**
 * yt8821_disable_broadcast - Disable MDIO broadcast on address 0
 */
static int yt8821_disable_broadcast(struct phy_device *phydev)
{
	int rc = 0;

	phy_lock_mdio_bus(phydev);

	rc = __phy_write(phydev, YTPHY_PAGE_SELECT, YTPHY_MDIO_ADDRESS_CONTROL_REG);
	if (rc < 0)
		goto unlock;

	rc = __phy_modify(phydev, YTPHY_PAGE_DATA, YTPHY_MACR_EN_PHY_ADDR_0, 0);

unlock:
	phy_unlock_mdio_bus(phydev);
	return rc;
}

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
	int rc;

	rc = phy_register_fixup_for_uid(PHY_ID_YT8821, 0xFFFFFFFF,
					yt8821_disable_broadcast);
	if (rc < 0)
		return rc;

	return 0;
}
