// SPDX-License-Identifier: GPL-2.0-only
/*
 * Class to manage OS controlled coherency agents within the system.
 * Specifically to enable operations such as write back and invalidate.
 *
 * Copyright: Huawei 2025
 */

#include <linux/cache_coherency.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/cacheflush.h>
static LIST_HEAD(cache_device_list);
static DEFINE_MUTEX(cache_device_list_lock);

void cache_coherency_device_free(struct cache_coherency_device *ccd)
{
	kfree(ccd);
}
EXPORT_SYMBOL_GPL(cache_coherency_device_free);

static int cache_inval_one(struct cache_coherency_device *ccd, void *data)
{
	if (!ccd->ops)
		return -EINVAL;

	return ccd->ops->wbinv(ccd, data);
}

static int cache_inval_done_one(struct cache_coherency_device *ccd, void *data)
{
	if (!ccd->ops)
		return -EINVAL;

	if (!ccd->ops->done)
		return 0;

	return ccd->ops->done(ccd);
}

static int cache_invalidate_memregion(int res_desc,
				      phys_addr_t addr, size_t size)
{
	int ret;
	struct cache_coherency_device *ccd;
	struct cc_inval_params params = {
		.addr = addr,
		.size = size,
	};
	guard(mutex)(&cache_device_list_lock);
	list_for_each_entry(ccd, &cache_device_list, node) {
		ret = cache_inval_one(ccd, &params);
		if (ret)
			return ret;
	}
	list_for_each_entry(ccd, &cache_device_list, node) {
		ret = cache_inval_done_one(ccd, &params);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct system_cache_flush_method cache_flush_method = {
	.invalidate_memregion = cache_invalidate_memregion,
};

struct cache_coherency_device *
cache_coherency_alloc_device(struct device *parent,
			const struct coherency_ops *ops, size_t size)
{

	if (!ops || !ops->wbinv)
		return NULL;

	struct cache_coherency_device *ccd __free(kfree) = kzalloc(size, GFP_KERNEL);

	if (!ccd)
		return NULL;

	ccd->parent = parent;
	ccd->ops = ops;
	INIT_LIST_HEAD(&ccd->node);

	return_ptr(ccd);
}
EXPORT_SYMBOL_NS_GPL(cache_coherency_alloc_device, "CACHE_COHERENCY");

int cache_coherency_device_register(struct cache_coherency_device *ccd)
{
	guard(mutex)(&cache_device_list_lock);
	list_add(&ccd->node, &cache_device_list);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cache_coherency_device_register, "CACHE_COHERENCY");

void cache_coherency_device_unregister(struct cache_coherency_device *ccd)
{
	guard(mutex)(&cache_device_list_lock);
	list_del(&ccd->node);
}
EXPORT_SYMBOL_NS_GPL(cache_coherency_device_unregister, "CACHE_COHERENCY");

static int __init cache_coherency_init(void)
{
	generic_set_sys_cache_flush_method(&cache_flush_method);

	return 0;
}
subsys_initcall(cache_coherency_init);
