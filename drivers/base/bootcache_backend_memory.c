// SPDX-License-Identifier: GPL-2.0
#define DEBUG 1
#include <linux/unaligned.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <linux/bootcache.h>

#define DRIVER_NAME "bootcache_memory_backend"
#define BOOTCACHE_MAGIC ('B' << 24 | 'C' << 16 | 'H' << 8 | 'E')
#define BOOTCACHE_MINSIZE 4096

/*
 * This defines a cache entry as stored.
 */
struct cache_memory_store_entry {
	u32 key_length;
	u32 data_length;
	u8 data_type;
	u8 data[];
} __packed;

/*
 * The in memory store of multiple cache entries.
 */
struct cache_memory_store {
	u32 magic;
	u32 entry_count;
	struct cache_memory_store_entry entries[];
} __packed;

struct reserved_mem *rmem;

/*
 * This function processes the loaded data and adds each entry to the
 * system cache via the callbck.
 */
static int memory_backend_load_cache(void)
{
	const struct cache_memory_store *store;
	const u8 *current_ptr;
	const void *max_address;
	u32 entry_count;
	int i;
	int ret;

	if (!rmem) {
		pr_warn("%s: No bootcache was found\n", DRIVER_NAME);
		return 0;
	}

	store = ioremap(rmem->base, rmem->size);
	if (!store) {
		pr_warn("%s: Unable to map reserved memory 0x%llx\n", DRIVER_NAME, rmem->base);
		return -ENOMEM;
	}
	max_address = store + rmem->size;
	current_ptr = (const unsigned char *)store->entries;
	entry_count = get_unaligned(&store->entry_count);

	for (i = 0; i < entry_count; i++) {
		struct cache_memory_store_entry *entry;
		struct bootcache_entry *new_entry = NULL;
		size_t data_length, key_length;
		u8 *src, *dest;
		int j;

		entry = (struct cache_memory_store_entry *)current_ptr;
		data_length = get_unaligned(&entry->data_length);
		key_length = get_unaligned(&entry->key_length);

		/* Check if will go outside the bounds */
		if ((current_ptr + sizeof(struct cache_memory_store_entry) +
				data_length + key_length + 1) > max_address) {
			ret = -ENOMEM;
			goto error;
		}

		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry) {
			ret = -ENOMEM;
			goto error;
		}

		new_entry->len = data_length;
		new_entry->key = kzalloc(key_length+1, GFP_KERNEL);
		new_entry->data = kzalloc(data_length, GFP_KERNEL);

		if (!new_entry->key || !new_entry->data) {
			pr_err("%s: Memory Allocation error creating new_entry data\n",
				DRIVER_NAME);
			kfree(new_entry->key);
			kfree(new_entry->data);
			kfree(new_entry);
			ret = -ENOMEM;
			goto error;
		}
		/*
		 * Source data is potentially unaligned, so we copy it with the correct
		 * access functions
		 */
		src = &entry->data[0];
		dest = new_entry->key;
		for (j = 0; j < key_length; j++)
			*dest++ = get_unaligned(src++);

		src = &entry->data[key_length+1];
		dest = new_entry->data;
		for (j = 0; j < data_length; j++)
			*dest++ = get_unaligned(src++);

		pr_debug("%s: Setting up Entry (%d) with key: %s, data length is %zu\n",
			DRIVER_NAME, i, new_entry->key, new_entry->len);

		/* call the framework provided function */
		ret = bootcache_add_entry(new_entry);
		if (ret) {
			kfree(new_entry->key);
			kfree(new_entry->data);
			kfree(new_entry);
			ret = 0;
		}

		/* Sanity check we've got space for the next extry */

		current_ptr += sizeof(struct cache_memory_store_entry) +
			data_length + key_length + 1;
		if (current_ptr + sizeof(struct cache_memory_store_entry)
				> max_address) {
			ret = ret = -ENOMEM;
			goto error;
		}
	}

error:
	if (store)
		iounmap((void *)store);

	return ret;
}

static struct bootcache_info cache_info = {
	.name = "memory",
	.load_cache = memory_backend_load_cache,
};

static int bootcache_backend_probe(struct platform_device *pdev)
{
	int ret;
	size_t table_size;
	struct cache_memory_store *temp_store;
	struct device_node *reserved_mem_node;

	/* Check for the front end being ready */

	pr_debug("%s: %s\n", DRIVER_NAME, __func__);

	reserved_mem_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (reserved_mem_node) {
		rmem = of_reserved_mem_lookup(reserved_mem_node);
		of_node_put(reserved_mem_node);
	}

	if (!rmem) {
		pr_err("%s: Failed to find reserved memory region.\n", DRIVER_NAME);
		return -ENOMEM;
	}
	pr_debug("%s: Found reserved cache memory block (%s):\n", DRIVER_NAME, rmem->name);
	pr_debug("%s:  Physical Address: 0x%llx\n", DRIVER_NAME, rmem->base);
	pr_debug("%s:  Size: 0x%llx (%llu bytes)\n", DRIVER_NAME, rmem->size,
		rmem->size);

	if (rmem->size < BOOTCACHE_MINSIZE) {
		pr_err("%s: reserved memory too small (%llu bytes)\n", DRIVER_NAME, rmem->size);
		return -ENOMEM;
	}

	ret = bootcache_register_backend(&cache_info);

	if (ret < 0) {
		pr_err("%s: bootcache_register_backend() failed with error %d\n",
			DRIVER_NAME, ret);
		return ret;
	}
	pr_info("%s: Backend loaded\n", DRIVER_NAME);

	return 0;
}

static const struct of_device_id bootcache_backend_driver_dt_ids[] = {
	{ .compatible = "linux,backend-backend-memory", },
	{ }
};

static struct platform_driver bootcache_memory_platform_driver = {
	.probe		= bootcache_backend_probe,
	.driver		= {
		.name	= DRIVER_NAME,
		.of_match_table = of_match_ptr(bootcache_backend_driver_dt_ids),
	},
};

static int __init bootcache_backend_init(void)
{
	return platform_driver_register(&bootcache_memory_platform_driver);
}

core_initcall(bootcache_backend_init);
