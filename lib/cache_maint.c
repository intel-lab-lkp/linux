// SPDX-License-Identifier: GPL-2.0
/*
 * Generic support for Memory System Cache Maintenance operations.
 *
 * Coherency maintenance drivers register with this simple framework that will
 * iterate over each registered instance to first kick off invalidation and
 * then to wait until it is complete.
 *
 * If no implementations are registered yet cpu_cache_has_invalidate_memregion()
 * will return false. If this runs concurrently with unregistration then a
 * race exists but this is no worse than the case where the device responsible
 * for a given memory region has not yet registered.
 */
#include <linux/cache_coherency.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/memregion.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/slab.h>

static LIST_HEAD(cache_device_list);
static DECLARE_RWSEM(cache_device_list_lock);

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

static int cache_inval_done_one(struct cache_coherency_device *ccd)
{
	if (!ccd->ops)
		return -EINVAL;

	if (!ccd->ops->done)
		return 0;

	return ccd->ops->done(ccd);
}

static int cache_invalidate_memregion(phys_addr_t addr, size_t size)
{
	int ret;
	struct cache_coherency_device *ccd;
	struct cc_inval_params params = {
		.addr = addr,
		.size = size,
	};

	guard(rwsem_read)(&cache_device_list_lock);
	list_for_each_entry(ccd, &cache_device_list, node) {
		ret = cache_inval_one(ccd, &params);
		if (ret)
			return ret;
	}
	list_for_each_entry(ccd, &cache_device_list, node) {
		ret = cache_inval_done_one(ccd);
		if (ret)
			return ret;
	}

	return 0;
}

struct cache_coherency_device *
_cache_coherency_device_alloc(const struct coherency_ops *ops, size_t size)
{
	struct cache_coherency_device *ccd;

	if (!ops || !ops->wbinv)
		return NULL;

	ccd = kzalloc(size, GFP_KERNEL);
	if (!ccd)
		return NULL;

	ccd->ops = ops;
	INIT_LIST_HEAD(&ccd->node);

	return ccd;
}
EXPORT_SYMBOL_NS_GPL(_cache_coherency_device_alloc, "CACHE_COHERENCY");

int cache_coherency_device_register(struct cache_coherency_device *ccd)
{
	guard(rwsem_write)(&cache_device_list_lock);
	list_add(&ccd->node, &cache_device_list);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cache_coherency_device_register, "CACHE_COHERENCY");

void cache_coherency_device_unregister(struct cache_coherency_device *ccd)
{
	guard(rwsem_write)(&cache_device_list_lock);
	list_del(&ccd->node);
}
EXPORT_SYMBOL_NS_GPL(cache_coherency_device_unregister, "CACHE_COHERENCY");

int cpu_cache_invalidate_memregion(phys_addr_t start, size_t len)
{
	return cache_invalidate_memregion(start, len);
}
EXPORT_SYMBOL_NS_GPL(cpu_cache_invalidate_memregion, "DEVMEM");

/*
 * Used for optimization / debug purposes only as removal can race
 *
 * Machines that do not support invalidation, e.g. VMs, will not
 * have any devices to register and so this will always return false.
 */
bool cpu_cache_has_invalidate_memregion(void)
{
	guard(rwsem_read)(&cache_device_list_lock);
	return !list_empty(&cache_device_list);
}
EXPORT_SYMBOL_NS_GPL(cpu_cache_has_invalidate_memregion, "DEVMEM");
