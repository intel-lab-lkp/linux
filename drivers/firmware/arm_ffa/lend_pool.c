// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm FF-A Reserved Memory CMA driver for Memory Lending
 *
 * Prevents CPU speculative reads to secure memory by unmapping it from the
 * kernel direct map. This only works if the reserved-memory is mapped at the
 * last-level ("ll-map;" or "rodata=full") or if all CPUs in the system support
 * BBML3.
 *
 * Copyright (C) 2026 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <linux/arm_ffa.h>
#include <linux/cleanup.h>
#include <linux/cma.h>
#include <linux/dma-map-ops.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/rcupdate.h>
#include <linux/set_memory.h>
#include <linux/xarray.h>

static DEFINE_XARRAY(ffa_lend_devices);

static bool ffa_lend_pool_contains(struct device *dev, phys_addr_t addr, size_t size)
{
	phys_addr_t base, end;

	if (!dev)
		return false;

	guard(rcu)();

	if (xa_load(&ffa_lend_devices, (unsigned long)dev) != dev)
		return false;

	if (WARN_ON_ONCE(!dev->cma_area))
		return false;

	base = cma_get_base(dev->cma_area);
	end = base + cma_get_size(dev->cma_area);

	return addr >= base && (addr + size) <= end;
}

/**
 * ffa_prepare_lend() - Prepare a memory region to be lent in FF-A
 * @dev:	Device attached to the lend pool
 * @addr:	Physical start address of the memory region
 * @size:	Size in bytes
 *
 * When memory is lent via FF-A, TrustZone transitions it to the secure state.
 * As long as Arm CPUs retain a valid mapping to that now-secure memory, they
 * can speculatively read it, which is fatal on some systems.
 *
 * ffa_prepare_lend() prevents this by unmapping the memory range from the
 * kernel's direct map.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int ffa_prepare_lend(struct device *dev, phys_addr_t addr, size_t size)
{
	unsigned long nr_pages = size >> PAGE_SHIFT;
	struct page *page;
	unsigned long i;
	int ret;

	if (!ffa_lend_pool_contains(dev, addr, size))
		return -ENODEV;

	page = pfn_to_page(PHYS_PFN(addr));
	for (i = 0; i < nr_pages; i++) {
		ret = __set_direct_map_invalid_noflush(page + i);

		if (ret) {
			while (i--)
				__set_direct_map_default_noflush(page + i);

			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ffa_prepare_lend);

/**
 * ffa_lend_reclaimed() - Restore a reclaimed FF-A memory region
 * @dev:	Device attached to the lend pool
 * @addr:	Physical start address of the memory region
 * @size:	Size in bytes
 *
 * Restores a memory range into the kernel's direct mapping. It must be called
 * after a successful FF-A memory reclaim invocation.
 */
void ffa_lend_reclaimed(struct device *dev, phys_addr_t addr, size_t size)
{
	unsigned long nr_pages = size >> PAGE_SHIFT;
	struct page *page;
	unsigned long i;

	if (!ffa_lend_pool_contains(dev, addr, size))
		return;

	page = pfn_to_page(PHYS_PFN(addr));
	for (i = 0; i < nr_pages; i++)
		__set_direct_map_default_noflush(page + i);
}
EXPORT_SYMBOL_GPL(ffa_lend_reclaimed);

/**
 * ffa_lend_pool_attach() - Attach a device to the FF-A lend pool
 * @dev:	Device to attach
 *
 * FF-A devices are dynamically discovered and might not have an associated
 * device tree node with a "memory-region" phandle. In that case, drivers must
 * use this function to attach to the "arm,ffa-lend-pool" reserved memory
 * region.
 *
 * Return: 0 on success, negative error code otherwise.
 */
int ffa_lend_pool_attach(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	np = of_find_compatible_node(NULL, NULL, "arm,ffa-lend-pool");
	if (!np)
		return -ENODEV;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem || !rmem->ops || !rmem->ops->device_init)
		return -EINVAL;

	return rmem->ops->device_init(rmem, dev);
}
EXPORT_SYMBOL_GPL(ffa_lend_pool_attach);

/**
 * ffa_lend_pool_detach() - Detach a device from the FF-A lend pool
 * @dev:	Device to detach
 *
 * Releases the device from the "arm,ffa-lend-pool" reserved memory region.
 */
void ffa_lend_pool_detach(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;

	np = of_find_compatible_node(NULL, NULL, "arm,ffa-lend-pool");
	if (!np)
		return;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);

	if (rmem && rmem->ops && rmem->ops->device_release)
		rmem->ops->device_release(rmem, dev);
}
EXPORT_SYMBOL_GPL(ffa_lend_pool_detach);

static int __init ffa_lend_pool_setup(unsigned long node, struct reserved_mem *rmem)
{
	struct cma *cma;
	int ret;

	if (!of_get_flat_dt_prop(node, "reusable", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL)) {
		pr_err("FF-A lend pool: node must be 'reusable' and not 'no-map'\n");
		return -EINVAL;
	}

	if (!IS_ALIGNED(rmem->base | rmem->size, CMA_MIN_ALIGNMENT_BYTES)) {
		pr_err("FF-A lend pool: incorrect alignment of CMA region\n");
		return -EINVAL;
	}

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0, rmem->name, &cma);
	if (ret) {
		pr_err("FF-A lend pool: unable to setup CMA region (%d)\n", ret);
		return ret;
	}

	rmem->priv = cma;

	return 0;
}

static int ffa_lend_pool_device_init(struct reserved_mem *rmem, struct device *dev)
{
	int ret;

	if (!can_set_direct_map_range(pfn_to_page(PHYS_PFN(rmem->base)), rmem->size / PAGE_SIZE)) {
		pr_err("FF-A lend pool: reserved memory cannot be unmapped in direct map\n");
		return -EINVAL;
	}

	dev->cma_area = rmem->priv;

	ret = xa_err(xa_store(&ffa_lend_devices, (unsigned long)dev, dev, GFP_KERNEL));
	if (ret)
		dev->cma_area = NULL;

	return ret;
}

static void ffa_lend_pool_device_release(struct reserved_mem *rmem, struct device *dev)
{
	xa_erase(&ffa_lend_devices, (unsigned long)dev);
	dev->cma_area = NULL;
}

static const struct reserved_mem_ops ffa_lend_pool_ops = {
	.node_init	= ffa_lend_pool_setup,
	.device_init	= ffa_lend_pool_device_init,
	.device_release = ffa_lend_pool_device_release,
};
RESERVEDMEM_OF_DECLARE(ffa_lend_pool, "arm,ffa-lend-pool", &ffa_lend_pool_ops);
