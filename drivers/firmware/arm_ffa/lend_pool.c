// SPDX-License-Identifier: GPL-2.0-only
/*
 * Arm FF-A Reserved Memory CMA driver for Memory Lending
 *
 * Prevents CPU speculative reads to secure memory by unmapping it from the
 * kernel direct map. This works if rodata=full or if all CPUs in the system
 * support BBML3 or if a reserved-memory is declared for this driver
 * "arm,ffa_lend_pool".
 *
 * Copyright (C) 2026 Google LLC
 * Author: Vincent Donnefort <vdonnefort@google.com>
 */

#include <linux/arm_ffa.h>
#include <linux/cleanup.h>
#include <linux/cma.h>
#include <linux/dma-map-ops.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/rcupdate.h>
#include <linux/suspend.h>
#include <linux/set_memory.h>
#include <linux/xarray.h>

#include <linux/cacheflush.h>
#include <asm/tlbflush.h>

static DEFINE_XARRAY(ffa_lend_pool_devices);
static atomic_t ffa_lend_pool_active;

#define FFA_LEND_POOL_DISABLED	-1

static bool ffa_lend_pool_can_set_direct_map(struct device *dev, struct page *page, u64 nr_pages)
{
	phys_addr_t addr, base, end;
	size_t size;

	if (can_set_direct_map())
		return true;

	if (!dev)
		return false;

	guard(rcu)();

	if (xa_load(&ffa_lend_pool_devices, (unsigned long)dev) != dev)
		return false;

	if (WARN_ON_ONCE(!dev->cma_area))
		return false;

	addr = page_to_phys(page);
	size = nr_pages << PAGE_SHIFT;
	base = cma_get_base(dev->cma_area);
	end = base + cma_get_size(dev->cma_area);

	return addr >= base && (addr + size) <= end;
}

/**
 * ffa_prepare_lend() - Prepare a memory region to be lent in FF-A
 * @dev:	Device attached to the lend pool
 * @page:	First page of the memory region
 * @nr_pages:	Number of pages
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
int ffa_prepare_lend(struct device *dev, struct page *page, u64 nr_pages)
{
	struct page *p, *end;
	unsigned long addr;
	int ret;

	if (!ffa_lend_pool_can_set_direct_map(dev, page, nr_pages))
		return -ENODEV;

	/* provides full ordering */
	if (!atomic_add_unless(&ffa_lend_pool_active, 1, FFA_LEND_POOL_DISABLED))
		return -EBUSY;

	end = page + nr_pages;
	for (p = page; p < end; p++)
		flush_dcache_page(p);

	ret = __set_direct_map_invalid_noflush(page, nr_pages);
	if (ret) {
		atomic_dec(&ffa_lend_pool_active);
		return ret;
	}

	addr = (unsigned long)page_address(page);
	flush_tlb_kernel_range(addr, addr + (nr_pages << PAGE_SHIFT));

	return 0;
}
EXPORT_SYMBOL_GPL(ffa_prepare_lend);

/**
 * ffa_lend_reclaimed() - Restore a reclaimed FF-A memory region
 * @dev:	Device attached to the lend pool
 * @page:	First page of the memory region
 * @nr_pages:	Number of pages
 *
 * Restores a memory range into the kernel's direct mapping. It must be called
 * after a successful FF-A memory reclaim invocation.
 */
void ffa_lend_reclaimed(struct device *dev, struct page *page, u64 nr_pages)
{
	if (!ffa_lend_pool_can_set_direct_map(dev, page, nr_pages))
		return;

	__set_direct_map_default_noflush(page, nr_pages);

	atomic_dec_return_release(&ffa_lend_pool_active);
}
EXPORT_SYMBOL_GPL(ffa_lend_reclaimed);

static int ffa_lend_pool_pm_notify(struct notifier_block *nb, unsigned long mode, void *data)
{
	/* Prevent hibernation which would try to access lent memory */
	switch (mode) {
	case PM_HIBERNATION_PREPARE:
		if (atomic_cmpxchg_acquire(&ffa_lend_pool_active, 0, FFA_LEND_POOL_DISABLED))
			return notifier_from_errno(-EBUSY);
		break;
	case PM_POST_HIBERNATION:
		atomic_set(&ffa_lend_pool_active, 0);
	}

	return NOTIFY_DONE;
}

static struct notifier_block ffa_lend_pool_pm_nb = {
	.notifier_call	= ffa_lend_pool_pm_notify,
};

static const struct reserved_mem_ops ffa_lend_pool_ops;

static struct reserved_mem *ffa_lend_pool_get_rmem(void)
{
	struct reserved_mem *rmem;
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "arm,ffa-lend-pool");
	if (!np)
		return NULL;

	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);

	if (WARN_ON_ONCE(rmem && rmem->ops != &ffa_lend_pool_ops))
		return NULL;

	return rmem;
}

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
	struct reserved_mem *rmem = ffa_lend_pool_get_rmem();

	return rmem ? rmem->ops->device_init(rmem, dev) : -ENODEV;
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
	struct reserved_mem *rmem = ffa_lend_pool_get_rmem();

	if (rmem)
		rmem->ops->device_release(rmem, dev);
}
EXPORT_SYMBOL_GPL(ffa_lend_pool_detach);

static int __init ffa_lend_pool_setup(unsigned long node, struct reserved_mem *rmem)
{
	struct cma *cma;
	int ret;

	/* Limited by ffa_lend_pool_active */
	if (rmem->size / PAGE_SIZE > INT_MAX)
		return -E2BIG;

	if (!IS_ALIGNED(rmem->base | rmem->size, CMA_MIN_ALIGNMENT_BYTES)) {
		pr_err("FF-A lend pool: incorrect alignment of CMA region\n");
		return -EINVAL;
	}

	ret = memblock_mark_ptemap(rmem->base, rmem->size);
	if (ret)
		return ret;

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0, rmem->name, &cma);
	if (ret) {
		pr_err("FF-A lend pool: unable to setup CMA region (%d)\n", ret);
		memblock_clear_ptemap(rmem->base, rmem->size);
		return ret;
	}

	register_pm_notifier(&ffa_lend_pool_pm_nb);
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

	ret = xa_err(xa_store(&ffa_lend_pool_devices, (unsigned long)dev, dev, GFP_KERNEL));
	if (ret)
		dev->cma_area = NULL;

	return ret;
}

static void ffa_lend_pool_device_release(struct reserved_mem *rmem, struct device *dev)
{
	xa_erase(&ffa_lend_pool_devices, (unsigned long)dev);
	dev->cma_area = NULL;
}

static const struct reserved_mem_ops ffa_lend_pool_ops = {
	.node_init	= ffa_lend_pool_setup,
	.device_init	= ffa_lend_pool_device_init,
	.device_release = ffa_lend_pool_device_release,
};
RESERVEDMEM_OF_DECLARE(ffa_lend_pool, "arm,ffa-lend-pool", &ffa_lend_pool_ops);
