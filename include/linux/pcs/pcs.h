/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_PCS_H
#define __LINUX_PCS_H

#include <linux/phy.h>
#include <linux/phylink.h>

static inline bool pcs_supports_interface(struct phylink_pcs *pcs,
					  phy_interface_t interface)
{
	return test_bit(interface, pcs->supported_interfaces);
}

#ifdef CONFIG_OF_PCS
/**
 * of_pcs_get - Retrieves a PCS from a device node
 * @np: Device node
 * @index: Index of PCS handle in Device Node
 * @interface: requested PHY interface type for PCS
 *
 * Get a PCS for the requested PHY interface type from the
 * device node at index.
 *
 * Returns a pointer to the phylink_pcs or a negative
 * error pointer. Can return -EPROBE_DEFER if the PCS is not
 * present in global providers list (either due to driver
 * still needs to be probed or it failed to probe/removed)
 */
struct phylink_pcs *of_pcs_get(struct device_node *np, int index,
			       phy_interface_t interface);

/**
 * of_phylink_mac_select_pcs - Generic MAC select pcs for OF PCS provider
 * @config: phylink config pointer
 * @interface: requested PHY interface type for PCS
 *
 * Generic helper function to get a PCS from a "pcs-handle" OF property
 * defined in device tree. Each phandle defined in "pcs-handle" will be
 * tested until a PCS that supports the requested PHY interface is found.
 *
 * Returns a pointer to the selected PCS or an error pointer.
 * Return NULL for PHY_INTERFACE_MODE_NA and a -EINVAL error pointer
 * for PHY_INTERFACE_MODE_INTERNAL. It can also return -EPROBE_DEFER,
 * refer to of_pcs_get for details about it.
 */
struct phylink_pcs *of_phylink_mac_select_pcs(struct phylink_config *config,
					      phy_interface_t interface);
#else
static inline struct phylink_pcs *of_pcs_get(struct device_node *np, int index,
					     phy_interface_t interface)
{
	return PTR_ERR(-ENOENT);
}

static inline struct phylink_pcs *of_phylink_mac_select_pcs(struct phylink_config *config,
							    phy_interface_t interface)
{
	return PTR_ERR(-EOPNOTSUPP);
}
#endif

#endif /* __LINUX_PCS_H */
