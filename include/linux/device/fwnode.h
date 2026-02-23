/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DEVICE_FWNODE_H_
#define _DEVICE_FWNODE_H_

#include <linux/stddef.h>

#include "types.h"

struct device_node;
struct fwnode_handle;

void set_primary_fwnode(struct device *dev, struct fwnode_handle *fwnode);
void set_secondary_fwnode(struct device *dev, struct fwnode_handle *fwnode);

void device_set_node(struct device *dev, struct fwnode_handle *fwnode);

int device_add_of_node(struct device *dev, struct device_node *of_node);
void device_remove_of_node(struct device *dev);
void device_set_of_node_from_dev(struct device *dev, const struct device *dev2);

static inline struct device_node *dev_of_node(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_OF) || !dev)
		return NULL;
	return dev->of_node;
}

#endif /* _DEVICE_FWNODE_H_ */
