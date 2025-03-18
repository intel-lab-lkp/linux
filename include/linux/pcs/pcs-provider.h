/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_PCS_PROVIDER_H
#define __LINUX_PCS_PROVIDER_H

#include <linux/phy.h>

/**
 * of_pcs_simple_get - Simple xlate function to retrieve PCS
 * @pcsspec: Phandle arguments
 * @data: Context data (assumed assigned to the single PCS)
 * @interface: requested PHY interface type for PCS
 *
 * Returns the PCS (pointed by data) or an -EOPNOTSUPP pointer
 * if the PCS doesn't support the requested interface.
 */
struct phylink_pcs *of_pcs_simple_get(struct of_phandle_args *pcsspec, void *data,
				      phy_interface_t interface);

/**
 * of_pcs_add_provider - Registers a new PCS provider
 * @np: Device node
 * @get: xlate function to retrieve the PCS
 * @data: Context data
 *
 * Register and add a new PCS to the global providers list
 * for the device node. A function to get the PCS from
 * device node with the use of phandle args.
 * To the get function is also passed the interface type
 * requested for the PHY. PCS driver will use the passed
 * interface to understand if the PCS can support it or not.
 *
 * Returns 0 on success or -ENOMEM on allocation failure.
 */
int of_pcs_add_provider(struct device_node *np,
			struct phylink_pcs *(*get)(struct of_phandle_args *pcsspec,
						   void *data,
						   phy_interface_t interface),
			void *data);

/**
 * of_pcs_del_provider - Removes a PCS provider
 * @np: Device node
 */
void of_pcs_del_provider(struct device_node *np);

#endif /* __LINUX_PCS_PROVIDER_H */
