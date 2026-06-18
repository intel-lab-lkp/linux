/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_PCS_H
#define __LINUX_PCS_H

#include <linux/phylink.h>

enum fwnode_pcs_notify_event {
	FWNODE_PCS_PROVIDER_ADD,
};

#if IS_ENABLED(CONFIG_FWNODE_PCS)
/**
 * register_fwnode_pcs_notifier - Register a notifier block for fwnode
 *				  PCS events
 * @nb: pointer to the notifier block
 *
 * Registers a notifier block to the fwnode_pcs_notify_list blocking
 * notifier chain. This allows phylink instance to subscribe for
 * PCS provider events.
 *
 * Returns: 0 or a negative error.
 */
int register_fwnode_pcs_notifier(struct notifier_block *nb);

/**
 * unregister_fwnode_pcs_notifier - Unregister a notifier block for fwnode
 *				    PCS events
 * @nb: pointer to the notifier block
 *
 * Unregisters a notifier block to the fwnode_pcs_notify_list blocking
 * notifier chain.
 *
 * Returns: 0 or a negative error.
 */
int unregister_fwnode_pcs_notifier(struct notifier_block *nb);

/**
 * fwnode_pcs_get - Retrieves a PCS from a firmware node
 * @fwnode: firmware node
 * @index: index fwnode PCS handle in firmware node
 *
 * Get a PCS from the firmware node at index.
 *
 * Returns: a pointer to the phylink_pcs or a negative
 * error pointer. Can return -ENODEV if the PCS is not
 * present in global providers list (either due to driver
 * still needs to be probed or it failed to probe/removed).
 */
struct phylink_pcs *fwnode_pcs_get(struct fwnode_handle *fwnode,
				   unsigned int index);

/**
 * fwnode_phylink_pcs_get_from_fwnode - Retrieves the PCS provided
 *					by the firmware node from a
 *					firmware node
 * @fwnode: firmware node
 * @pcs_fwnode: PCS firmware node
 *
 * Parse 'pcs-handle' in 'fwnode' and get the PCS that match
 * 'pcs_fwnode' firmware node.
 *
 * Returns: a pointer to the phylink_pcs or a negative
 * error pointer. Can return -EPROBE_DEFER if the PCS is not
 * present in global providers list (either due to driver
 * still needs to be probed or it failed to probe/removed)
 */
struct phylink_pcs *
fwnode_phylink_pcs_get_from_fwnode(struct fwnode_handle *fwnode,
				   struct fwnode_handle *pcs_fwnode);

/**
 * fwnode_phylink_pcs_count - count PCS entries described in firmware node
 * @fwnode: firmware node
 *
 * Helper function to count the number of PCS entries referenced by the
 * "pcs-handle" property in a firmware node.
 *
 * Note that this function counts all PCS references in the firmware node,
 * regardless of whether the corresponding PCS devices are already probed.
 *
 * Returns: number of PCS entries described in the firmware node.
 */
unsigned int fwnode_phylink_pcs_count(struct fwnode_handle *fwnode);

/**
 * fwnode_phylink_pcs_parse - parse available PCS from firmware node
 * @fwnode: firmware node
 * @available_pcs: pointer to preallocated array of PCS
 * @num_pcs: maximum number of PCS entries to scan
 *
 * Helper function that parses PCS references from the "pcs-handle"
 * property of a firmware node and fills @available_pcs with PCS that are
 * currently available up to @num_pcs.
 *
 * Only PCS that are currently available are stored in @available_pcs.
 * PCS that returns -ENODEV are skipped.
 *
 * Returns: number of PCS stored in @available_pcs, or negative error code.
 */
int fwnode_phylink_pcs_parse(struct fwnode_handle *fwnode,
			     struct phylink_pcs **available_pcs,
			     unsigned int num_pcs);
#else
static inline int register_fwnode_pcs_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline int unregister_fwnode_pcs_notifier(struct notifier_block *nb)
{
	return -EOPNOTSUPP;
}

static inline struct phylink_pcs *fwnode_pcs_get(struct fwnode_handle *fwnode,
						 unsigned int index)
{
	return ERR_PTR(-ENOENT);
}

static inline struct phylink_pcs *
fwnode_phylink_pcs_get_from_fwnode(struct fwnode_handle *fwnode,
				   struct fwnode_handle *pcs_fwnode)
{
	return ERR_PTR(-ENOENT);
}

static inline unsigned int fwnode_phylink_pcs_count(struct fwnode_handle *fwnode)
{
	return 0;
}

static inline int fwnode_phylink_pcs_parse(struct fwnode_handle *fwnode,
					   struct phylink_pcs **available_pcs,
					   unsigned int num_pcs)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __LINUX_PCS_H */
