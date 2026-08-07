/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_PCS_PROVIDER_H
#define __LINUX_PCS_PROVIDER_H

struct fwnode_pcs_provider;

/**
 * fwnode_pcs_simple_get - Simple xlate function to retrieve PCS
 * @pcsspec: reference arguments
 * @data: Context data (assumed assigned to the single PCS)
 *
 * Returns: the PCS pointed by data.
 */
struct phylink_pcs *fwnode_pcs_simple_get(struct fwnode_reference_args *pcsspec,
					  void *data);

/**
 * fwnode_pcs_add_provider - Registers a new PCS provider
 * @fwnode: Firmware node
 * @get: xlate function to retrieve the PCS
 * @data: Context data
 *
 * Register and add a new PCS provider to the global providers list
 * for the firmware node. The relevant PCS from the PCS provider
 * is retrieved from the passed xlate function.
 *
 * Returns: A pointer to the registered PCS provider on success, or
 * an ERR_PTR() encoded error code on failure.
 */
struct fwnode_pcs_provider *
fwnode_pcs_add_provider(struct fwnode_handle *fwnode,
			struct phylink_pcs *(*get)(struct fwnode_reference_args *pcsspec,
						   void *data),
			void *data);

/**
 * fwnode_pcs_del_provider - Removes a PCS provider
 * @fwnode: Firmware node
 */
void fwnode_pcs_del_provider(struct fwnode_pcs_provider *pp);

/**
 * fwnode_pcs_add_provider - Registers a new PCS provider
 * @dev: Device of the PCS provider
 * @fwnode: Firmware node
 * @get: xlate function to retrieve the PCS
 * @data: Context data
 *
 * Register and add a new PCS provider to the global providers list
 * for the firmware node. The relevant PCS from the PCS provider
 * is retrieved from the passed xlate function. While at that, it
 * also associates the device with the PCS provider using devres.
 * On driver detach, release function is invoked on the devres data,
 * then, devres data is freed.
 *
 * Returns: A pointer to the registered PCS provider on success, or
 * an ERR_PTR() encoded error code on failure.
 */
struct fwnode_pcs_provider *
devm_fwnode_pcs_add_provider(struct device *dev, struct fwnode_handle *fwnode,
			     struct phylink_pcs *(*get)(struct fwnode_reference_args *pcsspec,
							void *data),
			     void *data);

#endif /* __LINUX_PCS_PROVIDER_H */
