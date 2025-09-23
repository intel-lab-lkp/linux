// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <linux/bootcache.h>

#define DRIVER_NAME "bootcache_test_backend"

struct test_data {
	const char *name;
	u32 value;
	size_t data_length;
};

static struct test_data test_data_array[] = {
	{
		.name = "Bootcache Test One",
		.value = 1234,
		.data_length = sizeof(u32),
	},
	{
		.name = "Bootcache Test Two",
		.value = 5678,
		.data_length = sizeof(u32),
	},
	{
		.name = "Bootcache Test Three",
		.value = 9012,
		.data_length = sizeof(u32),
	},
	{
		.name = "Bootcache Test Four",
		.value = 0xDEADBEEF,
		.data_length = sizeof(u32),
	},
	{
		.name = "Bootcache Test Five",
		.value = 0xC0DEBAD0,
		.data_length = sizeof(u32),
	}
};

static int test_backend_load_cache(void)
{

	struct bootcache_entry *new_entry = NULL;
	int i;
	int ret;

	pr_info("%s: Backend local_cache callback\n", DRIVER_NAME);

	/*
	 * We want to load a bunch of fake data into the cache here
	 * so that it can be used for testing purposes
	 */
	for (i = 0; i < ARRAY_SIZE(test_data_array); i++) {
		/*
		 * Print the name and value of the current element.
		 * Use pr_info for a standard kernel log message.
		 */
		new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
		if (!new_entry)
			return -ENOMEM;

		new_entry->key = kstrdup(test_data_array[i].name, GFP_KERNEL);
		if (!new_entry->key) {
			kfree(new_entry);
			return -ENOMEM;
		}
		new_entry->len = test_data_array[i].data_length;
		new_entry->data = kmemdup(&test_data_array[i].value,
			test_data_array[i].data_length, GFP_KERNEL);
		if (!new_entry->data) {
			kfree(new_entry->key);
			kfree(new_entry);
			return -ENOMEM;
		}

		/* call the framework provided function */
		ret = bootcache_add_entry(new_entry);
		if (ret) {
			kfree(new_entry->key);
			kfree(new_entry->data);
			kfree(new_entry);
			ret = 0;
		}
	}
	return 0;
}

static struct bootcache_info cache_info = {
	.name = "test",
	.load_cache = test_backend_load_cache,
};

static int __init bootcache_backend_init(void)
{
	int ret;

	ret = bootcache_register_backend(&cache_info);

	if (ret < 0) {
		pr_err("%s: bootcache_register_backend() failed with error %d\n",
			DRIVER_NAME, ret);
		return ret;
	}
	pr_info("%s: Backend loaded\n", DRIVER_NAME);

	return 0;
}

core_initcall(bootcache_backend_init);
