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
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/nodemask.h>
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


/**
 * req_page_alloc_write() - Allocates the pages on the requested node, zone,
 * order and migrate type. Once the allocation is performed, a file is created
 * to free the allocation later.
 */
static ssize_t req_page_alloc_write(struct file *file, const char __user *ubuf,
				     size_t cnt, loff_t *ppos)
{
	return cnt;
}

static const struct file_operations req_page_alloc_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = req_page_alloc_write,
};

/**
 * create_nr_pages_allocs_file() - Creates the file "nr_pages_allocs".
 *
 * The "nr_pages_allocs" file will be used to write the number of allocations
 * that will be performed. When the allocations are performed, a new file for
 * each allocation will be created in @migratedir.
 */
static inline int create_nr_pages_allocs_file(struct dentry *migratedir,
					      int node_idx, int zone_idx,
					      int order, int mtype)
{
	struct req_alloc *req;
	struct dentry *nr_pages;

	req = kmem_cache_alloc(req_alloc_cache, GFP_KERNEL);
	if (!req) {
		pr_err("Failed to create nr_pages_allocs_info");
		return -ENOMEM;
	}

	req->node_idx = node_idx;
	req->zone_idx = zone_idx;
	req->order = order;
	req->migrate_type = mtype;
	req->parentdir = migratedir;

	nr_pages = debugfs_create_file("nr_pages_allocs", 0644, migratedir, req,
				       &req_page_alloc_fops);
	if (IS_ERR(nr_pages))
		return PTR_ERR(nr_pages);

	return 0;
}

/**
 * create_migrate_type_subdirs() - Creates a directory for each migrate type
 * inside the order directory. Once the migrate type directory is created, it
 * creates the "nr_allocs" file and the "free" file.
 */
static inline int create_migrate_type_subdirs(struct dentry *orderdir,
					      int node_idx, int zone_idx,
					      int order)
{
	struct dentry *migratedir;
	char dirname[24];
	int ret;

	for (int mtype = 0; mtype < MIGRATE_TYPES; mtype++) {
#ifdef CONFIG_CMA
		if (mtype == MIGRATE_CMA) /* CMA allocs are not supported yet*/
			continue;
#endif
#ifdef CONFIG_MEMORY_ISOLATION
		if (mtype == MIGRATE_ISOLATE) /* can't allocate from here */
			continue;
#endif
		snprintf(dirname, sizeof(dirname), "migrate-%s",
			 migratetype_names[mtype]);
		migratedir = debugfs_create_dir(dirname, orderdir);
		if (IS_ERR(migratedir))
			return PTR_ERR(migratedir);

		ret = create_nr_pages_allocs_file(migratedir, node_idx,
						  zone_idx, order, mtype);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * create_page_orders_subdirs() - Creates a directory for each page order inside
 * the zone directory. Once that the page order directory is created, it creates
 * a directory for each migrate type.
 */
static inline int create_page_orders_subdirs(struct dentry *zonedir,
					     int node_idx, int zone_idx,
					     struct zone *zone)
{
	struct dentry *orderdir;
	char dirname[12];
	int ret;

	for (int order = 0; order < NR_PAGE_ORDERS; order++) {
		snprintf(dirname, sizeof(dirname), "order-%d", order);
		orderdir = debugfs_create_dir(dirname, zonedir);
		if (IS_ERR(orderdir))
			return PTR_ERR(orderdir);

		ret = create_migrate_type_subdirs(orderdir, node_idx, zone_idx,
						  order);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * create_zones_subdirs() - Creates a directory for each populated zone in the
 * node. Once that the zone directory is created, it creates a directory for
 * each order supported.
 *
 * Note: ZONE_DEVICE pages won't be allocated using this driver. The main reason
 * is because they are not managed by the Buddy Allocator or CMA allocator.
 */
static inline int create_zones_subdirs(struct dentry *nodedir, int node_idx,
				       struct pglist_data *pgdata)
{
	struct dentry *zonedir;
	struct zone *zone;
	struct zone *node_zones = pgdata->node_zones;
	int zone_idx;
	char dirname[24];
	int ret;

	for (zone = node_zones, zone_idx = 0; zone - node_zones < MAX_NR_ZONES;
	     ++zone, ++zone_idx) {
		if (!populated_zone(zone))
			continue;

		/* ZONE_DEVICE pages won't be allocated using this driver. */
		if (zone_is_zone_device(zone))
			continue;

		snprintf(dirname, sizeof(dirname), "zone-%s", zone->name);
		zonedir = debugfs_create_dir(dirname, nodedir);
		if (IS_ERR(zonedir))
			return PTR_ERR(zonedir);

		ret = create_page_orders_subdirs(zonedir, node_idx, zone_idx,
						 zone);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * create_nodes_subdirs() - Creates a directory for each online node in the
 * system. Once that the node directory is created, it creates a directory for
 * each zone in the node.
 */
static inline int create_nodes_subdirs(struct dentry *mmdir)
{
	struct dentry *nodedir;
	int node_idx;
	char dirname[12];
	int ret;

	for_each_online_node(node_idx) {
		struct pglist_data *pgdata = NODE_DATA(node_idx);

		snprintf(dirname, sizeof(dirname), "node-%d", node_idx);
		nodedir = debugfs_create_dir(dirname, mmdir);
		if (IS_ERR(nodedir))
			return PTR_ERR(nodedir);

		ret = create_zones_subdirs(nodedir, node_idx, pgdata);
		if (ret)
			return ret;
	}

	return 0;
}

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

	ret = create_nodes_subdirs(mmdir);
	if (ret)
		goto clean_dir;

	return 0;

clean_dir:
	debugfs_remove_recursive(mmdir);

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
