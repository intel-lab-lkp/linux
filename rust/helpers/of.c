// SPDX-License-Identifier: GPL-2.0

#include <linux/of.h>

bool rust_helper_is_of_node(const struct fwnode_handle *fwnode)
{
	return is_of_node(fwnode);
}

struct device_node *rust_helper_to_of_node(const struct fwnode_handle *fwnode)
{
	return to_of_node(fwnode);
}
