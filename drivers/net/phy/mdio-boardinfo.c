// SPDX-License-Identifier: GPL-2.0+
/*
 * mdio-boardinfo - Collect pre-declarations for MDIO devices
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/list.h>

#include "mdio-boardinfo.h"

LIST_HEAD(mdio_board_list);
EXPORT_SYMBOL_GPL(mdio_board_list);
DEFINE_MUTEX(mdio_board_lock);
EXPORT_SYMBOL_GPL(mdio_board_lock);

/**
 * mdiobus_register_board_info - register MDIO devices for a given board
 * @info: array of devices descriptors
 * @n: number of descriptors provided
 * Context: can sleep
 *
 * The board info passed can be marked with __initdata but be pointers
 * such as platform_data etc. are copied as-is
 */
int mdiobus_register_board_info(const struct mdio_board_info *info,
				unsigned int n)
{
	struct mdio_board_entry *be;
	unsigned int i;

	be = kcalloc(n, sizeof(*be), GFP_KERNEL);
	if (!be)
		return -ENOMEM;

	for (i = 0; i < n; i++, be++, info++) {
		memcpy(&be->board_info, info, sizeof(*info));
		mutex_lock(&mdio_board_lock);
		list_add_tail(&be->list, &mdio_board_list);
		mutex_unlock(&mdio_board_lock);
	}

	return 0;
}
EXPORT_SYMBOL(mdiobus_register_board_info);
