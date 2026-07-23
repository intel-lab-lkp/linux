// SPDX-License-Identifier: GPL-2.0
/*
 * Page allocator DebugFS Interface
 * Author: Juan Yescas <jyescas@google.com>
 *
 * This module allows to make page allocs per node, zone, migrate type
 * and order using the DebugFS filesystem.
 *
 * When this module is installed and the debugfs is mounted, the "mm" directory
 " will be created in <debugfs mount point>/mm" directory. Under this directory,
 * there will be subdirs to select the nodes, zones, orders and migrate type.
 *
 * For example:
 *
 *      /sys/kernel/debug/mm/
 *      |-- free
 *      `-- node-0
 *         |-- zone-DMA
 *         |   |-- order-0
 *         |   |   |-- migrate-HighAtomic
 *         |   |   |   `-- nr_pages_allocs
 *         |   |   |-- migrate-Movable
 *         |   |   |   `-- nr_pages_allocs
 *         |   |   |-- migrate-Reclaimable
 *         |   |   |   `-- nr_pages_allocs
 *         |   |   `-- migrate-Unmovable
 *         |   |       `-- nr_pages_allocs
 *         |   |-- ...
 *         |   |
 *         |   `-- order-9
 *         |       |-- migrate-HighAtomic
 *         |       |   `-- nr_pages_allocs
 *         |       |-- migrate-Movable
 *         |       |   `-- nr_pages_allocs
 *         |       |-- migrate-Reclaimable
 *         |       |   `-- nr_pages_allocs
 *         |       `-- migrate-Unmovable
 *         |           `-- nr_pages_allocs
 *         `-- zone-Normal
 *             |-- order-0
 *             |   |-- migrate-HighAtomic
 *             |   |   `-- nr_pages_allocs
 *             |   |-- migrate-Movable
 *             |   |   `-- nr_pages_allocs
 *             |   |-- migrate-Reclaimable
 *             |   |   `-- nr_pages_allocs
 *             |   `-- migrate-Unmovable
 *             |       `-- nr_pages_allocs
 *             |-- ....
 *             |
 *             `-- order-9
 *                 |-- migrate-HighAtomic
 *                 |   `-- nr_pages_allocs
 *                 |-- migrate-Movable
 *                 |   `-- nr_pages_allocs
 *                 |-- migrate-Reclaimable
 *                 |   `-- nr_pages_allocs
 *                 `-- migrate-Unmovable
 *                     `-- nr_pages_allocs
 *
 * Usage:
 *
 * 1. To trigger the allocation, navigate to the debugfs path corresponding
 *    to your target node, memory zone, allocation order, and migration type,
 *    then write the requested allocation count to nr_pages_allocs.
 *
 *    For example, to make 3 allocs of order 9, Migrate Type Movable,
 *    Zone Normal and Node 0, run:
 *
 *    $ echo 3 > /sys/kernel/debug/mm/node-0/zone-Normal/order-9/migrate-Movable/nr_pages_allocs
 *
 * 2. For each allocation created, a corresponding file named sequentially
 *    (1, 2, n) will appear in that directory.
 *
 *    $ ls /sys/kernel/debug/mm/node-0/zone-Normal/order-9/migrate-Movable/
 *    1   2   3  nr_pages_allocs
 *
 * 3. To free the allocation, write the allocation file name in /sys/kernel/debug/mm/free.
 *    For example, to release the 2nd allocation run:
 *
 *    $ echo 2 > /sys/kernel/debug/mm/free
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>

struct dentry *mmdir;

static int __init page_alloc_hogger_debugfs_init(void)
{
	mmdir = debugfs_create_dir("mm", NULL);
	if (IS_ERR(mmdir)) {
		pr_err("Unable to create mm directory");
		return PTR_ERR(mmdir);
	}

	return 0;
}

static void __exit page_alloc_hogger_debugfs_exit(void)
{
	debugfs_remove_recursive(mmdir);
}

module_init(page_alloc_hogger_debugfs_init);
module_exit(page_alloc_hogger_debugfs_exit);

MODULE_AUTHOR("Juan Yescas");
MODULE_DESCRIPTION("Module to alloc pages to generate memory pressure");
MODULE_LICENSE("GPL");
