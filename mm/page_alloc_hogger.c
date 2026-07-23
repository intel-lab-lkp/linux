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
#include <linux/gfp.h>
#include <linux/gfp_types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>

struct dentry *mmdir;

/**
 * struct req_alloc - Represents the requested allocation
 * @node_idx: The Node index to allocate from.
 * @zone_idx: The Zone index to allocate from.
 * @order: The Order of pages to allocate.
 * @migrate_type: The Migrate type to allocate from.
 * @parentdir: The parent dir where the file representing the alloc was created.
 */
struct req_alloc {
	int node_idx;
	int zone_idx;
	int order;
	int migrate_type;
	struct dentry *parentdir;
};

/*
 * struct page_alloc - Represents the page allocation.
 * @page: The pointer to the page(s) allocated.
 * @req_alloc: The details of the allocation.
 * @alloc_dentry: The pointer to the file that represents the allocation.
 */
struct page_alloc {
	struct page *page;
	struct req_alloc *req_alloc;
	struct dentry *alloc_dentry;
};

struct kmem_cache *req_alloc_cache;
struct kmem_cache *page_alloc_cache;

static int __init page_alloc_hogger_debugfs_init(void)
{
	int ret;

	req_alloc_cache = kmem_cache_create(
		"req_alloc_cache", sizeof(struct req_alloc), 0,
		SLAB_HWCACHE_ALIGN, NULL);
	if (!req_alloc_cache) {
		pr_err("The req_alloc_cache couldn't be created");
		ret = -ENOMEM;
		goto error_exit;
	}

	page_alloc_cache = kmem_cache_create(
		"page_alloc_cache", sizeof(struct page_alloc), 0,
		SLAB_HWCACHE_ALIGN, NULL);
	if (!page_alloc_cache) {
		pr_err("The page_alloc_cache couldn't be created");
		ret = -ENOMEM;
		goto clean_req_alloc_cache;
	}

	mmdir = debugfs_create_dir("mm", NULL);
	if (IS_ERR(mmdir)) {
		pr_err("Unable to create mm directory");
		ret = PTR_ERR(mmdir);
		goto clean_page_alloc_cache;
	}

	return 0;


clean_page_alloc_cache:
	kmem_cache_destroy(page_alloc_cache);

clean_req_alloc_cache:
	kmem_cache_destroy(req_alloc_cache);

error_exit:
	return ret;
}
static void __exit page_alloc_hogger_debugfs_exit(void)
{
	debugfs_remove_recursive(mmdir);
	kmem_cache_destroy(req_alloc_cache);
	kmem_cache_destroy(page_alloc_cache);
}

module_init(page_alloc_hogger_debugfs_init);
module_exit(page_alloc_hogger_debugfs_exit);

MODULE_AUTHOR("Juan Yescas");
MODULE_DESCRIPTION("Module to alloc pages to generate memory pressure");
MODULE_LICENSE("GPL");
