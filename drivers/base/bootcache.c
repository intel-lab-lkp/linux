// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/stringhash.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/bootcache.h>

static DEFINE_HASHTABLE(bootcache_table, BOOTCACHE_HASH_BITS);
static DEFINE_SPINLOCK(bootcache_lock);
static struct kobject *bootcache_kobj;
static struct bootcache_info *bootcache_backend;
static bool bootcache_initilized;

int bootcache_register_backend(struct bootcache_info *bci)
{
	int ret;

	if (!bci)
		return -EINVAL;
	if (!bci->name)
		return -EINVAL;

	/* If we're not ready, tell backend to try again later */
	if (!bootcache_initilized)
		return -EPROBE_DEFER;

	if (bootcache_backend) {
		pr_warn("bootcache: Backend '%s' is already registered, cannot register '%s'\n",
		bootcache_backend->name, bci->name);
		return -EBUSY;
	}
	pr_info("bootcache: Registering backend '%s'\n",
		bci->name);

	/* Have the backend load and populate the cache store */
	ret = bci->load_cache();

	if (ret)
		goto failed_initilize;

	bootcache_backend = bci;
	return 0;

failed_initilize:
	return ret;
}
EXPORT_SYMBOL(bootcache_register_backend);

int bootcache_get(const char *name, void *buf, size_t *len)
{
	struct bootcache_entry *entry;
	u32 hash;
	int ret = -ENOENT;

	if (!name || !buf || !len)
		return -EINVAL;

	hash = full_name_hash(NULL, name, strlen(name));

	spin_lock(&bootcache_lock);
	hash_for_each_possible(bootcache_table, entry, node, hash) {
		if (strcmp(entry->key, name) == 0) {
			if (*len < entry->len) {
				*len = entry->len;
				ret = -ENOSPC;
				goto unlock;
			}
			memcpy(buf, entry->data, entry->len);
			*len = entry->len;
			ret = 0;
			goto unlock;
		}
	}

unlock:
	spin_unlock(&bootcache_lock);
	return ret;
}
EXPORT_SYMBOL(bootcache_get);

int bootcache_add_entry(struct bootcache_entry *entry)
{
	u32 hash;
	struct bootcache_entry *existing_entry;
	int ret = 0;

	hash = full_name_hash(NULL, entry->key, strlen(entry->key));

	spin_lock(&bootcache_lock);

	hash_for_each_possible(bootcache_table, existing_entry, node, hash) {
		if (strcmp(existing_entry->key, entry->key) == 0) {
			ret = -EEXIST;  // Key already exists
			goto unlock;
		}
	}

	hash_add(bootcache_table, &entry->node, hash);

unlock:
	spin_unlock(&bootcache_lock);
	return ret;
}
EXPORT_SYMBOL(bootcache_add_entry);

int bootcache_set(const char *name, const void *data, size_t len)
{
	struct bootcache_entry *new_entry;
	u32 hash;
	int ret = 0;

	if (!name || !data || !len)
		return -EINVAL;

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;

	new_entry->key = kstrdup(name, GFP_KERNEL);
	if (!new_entry->key) {
		ret = -ENOMEM;
		goto free;
	}

	new_entry->data = kmemdup(data, len, GFP_KERNEL);
	if (!new_entry->data) {
		ret = -ENOMEM;
		goto free;
	}

	new_entry->len = len;
	ret = bootcache_add_entry(new_entry);
	if (!ret)
		return 0;

free:
	kfree(new_entry->data);
	kfree(new_entry->key);
	kfree(new_entry);
	return ret;
}
EXPORT_SYMBOL(bootcache_set);

static ssize_t writeout_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	/*Implement persistent storage backend */
	return count;
}

static struct kobj_attribute writeout_attr = __ATTR_WO(writeout);

static int __init bootcache_init(void)
{
	int ret;

	pr_info("bootcache: backend loaded\n");

	/* Create /sys/kernel/bootcache/writeout */
	bootcache_kobj = kobject_create_and_add("bootcache", kernel_kobj);
	if (!bootcache_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(bootcache_kobj, &writeout_attr.attr);
	if (ret) {
		kobject_put(bootcache_kobj);
		return ret;
	}
	bootcache_initilized = true;
	return 0;
}
core_initcall(bootcache_init);
