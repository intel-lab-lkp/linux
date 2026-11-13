// SPDX-License-Identifier: GPL-2.0

#include <linux/configfs.h>

__rust_helper void
rust_helper_configfs_add_default_group(struct config_group *new_group,
				       struct config_group *group)
{
	configfs_add_default_group(new_group, group);
}
