// SPDX-License-Identifier: GPL-2.0

#include <linux/of.h>

__rust_helper bool rust_helper_is_of_node(const struct fwnode_handle *fwnode)
{
	return is_of_node(fwnode);
}

__rust_helper struct device_node *rust_helper_of_node_get(struct device_node *node)
{
	return of_node_get(node);
}

__rust_helper void rust_helper_of_node_put(struct device_node *node)
{
	of_node_put(node);
}
