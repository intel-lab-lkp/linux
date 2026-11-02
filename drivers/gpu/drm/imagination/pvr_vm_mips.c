// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_gem.h"
#define PVR_FW_MIPS_INTERNAL_H_EXPLICIT_INCLUDE
#include "pvr_fw_mips_internal.h"
#undef PVR_FW_MIPS_INTERNAL_H_EXPLICIT_INCLUDE
#include "pvr_mmu.h"
#include "pvr_vm.h"
#include "pvr_vm_mips.h"

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#define PVR_MIPS_PT_PAGE_COUNT \
	DIV_ROUND_UP(PVR_MIPSFW_MAX_NUM_PAGETABLE_PAGES * PVR_MIPSFW_PAGE_SIZE_4K, PAGE_SIZE)

/**
 * struct pvr_vm_mips_data - MIPS-VM-specific data
 */
struct pvr_vm_mips_data {
	/**
	 * @pt_pages: Pages containing MIPS pagetable.
	 */
	struct page *pt_pages[PVR_MIPS_PT_PAGE_COUNT];

	/** @pt: Pointer to CPU mapping of MIPS pagetable. */
	u32 *pt;

	/** @pt_dma_addr: DMA mappings of MIPS pagetable. */
	dma_addr_t pt_dma_addr[PVR_MIPS_PT_PAGE_COUNT];

	/** @cache_policy: Cache policy for this processor. */
	u32 cache_policy;

	/** @pfn_mask: PFN mask for MIPS pagetable. */
	u32 pfn_mask;
};

/**
 * pvr_vm_mips_init() - Initialise MIPS FW pagetable
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EINVAL,
 *  * Any error returned by pvr_gem_object_create(), or
 *  * And error returned by pvr_gem_object_vmap().
 */
int
pvr_vm_mips_init(struct pvr_device *pvr_dev)
{
	u32 pt_size = 1 << PVR_MIPSFW_LOG2_PAGETABLE_SIZE_4K(pvr_dev);
	struct device *dev = from_pvr_device(pvr_dev)->dev;
	struct pvr_vm_mips_data *vm_data;
	u32 phys_bus_width;
	int page_nr;
	int err;

	/* Page table size must be at most PVR_MIPSFW_MAX_NUM_PAGETABLE_PAGES * 4k pages. */
	if (pt_size > PVR_MIPSFW_MAX_NUM_PAGETABLE_PAGES * SZ_4K)
		return -EINVAL;

	if (PVR_FEATURE_VALUE(pvr_dev, phys_bus_width, &phys_bus_width))
		return -EINVAL;

	vm_data = drmm_kzalloc(from_pvr_device(pvr_dev), sizeof(*vm_data), GFP_KERNEL);
	if (!vm_data)
		return -ENOMEM;

	for (page_nr = 0; page_nr < PVR_MIPS_PT_PAGE_COUNT; page_nr++) {
		vm_data->pt_pages[page_nr] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!vm_data->pt_pages[page_nr]) {
			err = -ENOMEM;
			goto err_free_pages;
		}

		vm_data->pt_dma_addr[page_nr] = dma_map_page(dev, vm_data->pt_pages[page_nr], 0,
							     PAGE_SIZE, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, vm_data->pt_dma_addr[page_nr])) {
			err = -ENOMEM;
			__free_page(vm_data->pt_pages[page_nr]);
			goto err_free_pages;
		}
	}

	vm_data->pt = vmap(vm_data->pt_pages, pt_size >> PAGE_SHIFT, VM_MAP,
			   pgprot_writecombine(PAGE_KERNEL));
	if (!vm_data->pt) {
		err = -ENOMEM;
		goto err_free_pages;
	}

	if (phys_bus_width > 32) {
		vm_data->pfn_mask = PVR_MIPSFW_ENTRYLO_PFN_ABOVE_32BIT;
		vm_data->cache_policy = PVR_MIPSFW_CACHE_POLICY_CACHED_ABOVE_32BIT;
	} else {
		vm_data->pfn_mask = PVR_MIPSFW_ENTRYLO_PFN;
		vm_data->cache_policy = PVR_MIPSFW_CACHE_POLICY_CACHED;
	}

	pvr_dev->fw_dev.processor_data.mips_data.vm_data = vm_data;

	return 0;

err_free_pages:
	while (--page_nr >= 0) {
		dma_unmap_page(from_pvr_device(pvr_dev)->dev,
			       vm_data->pt_dma_addr[page_nr], PAGE_SIZE, DMA_TO_DEVICE);

		__free_page(vm_data->pt_pages[page_nr]);
	}

	return err;
}

/**
 * pvr_vm_mips_fini() - Release MIPS FW pagetable
 * @pvr_dev: Target PowerVR device.
 */
void
pvr_vm_mips_fini(struct pvr_device *pvr_dev)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct pvr_vm_mips_data *vm_data = fw_dev->processor_data.mips_data.vm_data;

	vunmap(vm_data->pt);
	for (int page_nr = PVR_MIPS_PT_PAGE_COUNT - 1; page_nr >= 0; page_nr--) {
		dma_unmap_page(from_pvr_device(pvr_dev)->dev,
			       vm_data->pt_dma_addr[page_nr], PAGE_SIZE, DMA_TO_DEVICE);

		__free_page(vm_data->pt_pages[page_nr]);
	}

	fw_dev->processor_data.mips_data.vm_data = NULL;
}

static u32
get_mips_pte_flags(bool read, bool write, u32 cache_policy)
{
	u32 flags = 0;

	if (read && write) /* Read/write. */
		flags |= PVR_MIPSFW_ENTRYLO_DIRTY;
	else if (write)    /* Write only. */
		flags |= PVR_MIPSFW_ENTRYLO_READ_INHIBIT;
	else
		WARN_ON(!read);

	flags |= FIELD_PREP(PVR_MIPSFW_ENTRYLO_CACHE_POLICY, cache_policy);

	flags |= PVR_MIPSFW_ENTRYLO_VALID | PVR_MIPSFW_ENTRYLO_GLOBAL;

	return flags;
}

/**
 * pvr_vm_mips_map() - Map a FW object into MIPS address space
 * @pvr_dev: Target PowerVR device.
 * @fw_obj: FW object to map.
 *
 * Returns:
 *  * 0 on success,
 *  * -%EINVAL if object does not reside within FW address space, or
 *  * Any error returned by pvr_fw_object_get_dma_addr().
 */
int
pvr_vm_mips_map(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct pvr_vm_mips_data *vm_data = fw_dev->processor_data.mips_data.vm_data;
	struct pvr_gem_object *pvr_obj = fw_obj->gem;
	const u64 start = fw_obj->fw_mm_node.start;
	const u64 size = fw_obj->fw_mm_node.size;
	u64 end;
	u32 cache_policy;
	u32 pte_flags;
	s32 start_pfn;
	s32 end_pfn;
	s32 pfn;
	int err;

	if (check_add_overflow(start, size - 1, &end))
		return -EINVAL;

	if (start < ROGUE_FW_HEAP_BASE ||
	    start >= ROGUE_FW_HEAP_BASE + fw_dev->fw_heap_info.raw_size ||
	    end < ROGUE_FW_HEAP_BASE ||
	    end >= ROGUE_FW_HEAP_BASE + fw_dev->fw_heap_info.raw_size ||
	    (start & PVR_MIPSFW_PAGE_MASK_4K) ||
	    ((end + 1) & PVR_MIPSFW_PAGE_MASK_4K))
		return -EINVAL;

	start_pfn = (start & fw_dev->fw_heap_info.offset_mask) >> PVR_MIPSFW_LOG2_PAGE_SIZE_4K;
	end_pfn = (end & fw_dev->fw_heap_info.offset_mask) >> PVR_MIPSFW_LOG2_PAGE_SIZE_4K;

	if (pvr_obj->flags & PVR_BO_FW_FLAGS_DEVICE_UNCACHED)
		cache_policy = PVR_MIPSFW_CACHE_POLICY_UNCACHED;
	else
		cache_policy = vm_data->cache_policy;

	pte_flags = get_mips_pte_flags(true, true, cache_policy);

	for (pfn = start_pfn; pfn <= end_pfn; pfn++) {
		dma_addr_t dma_addr;
		u32 pte;

		err = pvr_fw_object_get_dma_addr(fw_obj,
						 (pfn - start_pfn) <<
						 PVR_MIPSFW_LOG2_PAGE_SIZE_4K,
						 &dma_addr);
		if (err)
			goto err_unmap_pages;

		pte = field_prep(vm_data->pfn_mask, dma_addr >> PVR_MIPSFW_LOG2_PAGE_SIZE_4K);
		pte |= pte_flags;

		WRITE_ONCE(vm_data->pt[pfn], pte);
	}

	pvr_mmu_flush_request_all(pvr_dev);

	return 0;

err_unmap_pages:
	while (--pfn >= start_pfn)
		WRITE_ONCE(vm_data->pt[pfn], 0);

	pvr_mmu_flush_request_all(pvr_dev);
	WARN_ON(pvr_mmu_flush_exec(pvr_dev, true));

	return err;
}

/**
 * pvr_vm_mips_unmap() - Unmap a FW object into MIPS address space
 * @pvr_dev: Target PowerVR device.
 * @fw_obj: FW object to unmap.
 */
void
pvr_vm_mips_unmap(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct pvr_vm_mips_data *vm_data = fw_dev->processor_data.mips_data.vm_data;
	const u64 start = fw_obj->fw_mm_node.start;
	const u64 size = fw_obj->fw_mm_node.size;
	const u64 end = start + size;

	const u32 start_pfn = (start & fw_dev->fw_heap_info.offset_mask) >>
			      PVR_MIPSFW_LOG2_PAGE_SIZE_4K;
	const u32 end_pfn = (end & fw_dev->fw_heap_info.offset_mask) >>
			    PVR_MIPSFW_LOG2_PAGE_SIZE_4K;

	for (u32 pfn = start_pfn; pfn < end_pfn; pfn++)
		WRITE_ONCE(vm_data->pt[pfn], 0);

	pvr_mmu_flush_request_all(pvr_dev);
	WARN_ON(pvr_mmu_flush_exec(pvr_dev, true));
}

/**
 * pvr_vm_get_pt_phys_addrs() - Acquire the physical addresses of the MIPS FW page table pages
 * @pvr_dev: Target PowerVR device.
 * @phys_addrs: Output array for physical addresses of MIPS FW page table pages.
 * @max_num_addrs: Maximum number of addresses that can be written to @phys_addrs.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%EINVAL if @phys_addrs cannot accommodate all MIPS FW page table pages.
 */
int
pvr_vm_get_pt_phys_addrs(struct pvr_device *pvr_dev, u64 *phys_addrs, int max_num_addrs)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	struct pvr_vm_mips_data *vm_data = fw_dev->processor_data.mips_data.vm_data;

	if (ARRAY_SIZE(vm_data->pt_pages) > max_num_addrs) {
		drm_err(from_pvr_device(pvr_dev),
			"Minimum %zu pages required for FW PT, but only %d DMA address slots were provided\n",
			ARRAY_SIZE(vm_data->pt_pages), max_num_addrs);
		return -EINVAL;
	}

	for (u32 page_nr = 0; page_nr < max_num_addrs; page_nr++) {
		/* Firmware expects 4k pages, but host page size might be different. */
		u32 src_page_nr = (page_nr * PVR_MIPSFW_PAGE_SIZE_4K) >> PAGE_SHIFT;
		u32 page_offset = (page_nr * PVR_MIPSFW_PAGE_SIZE_4K) & ~PAGE_MASK;

		phys_addrs[page_nr] = vm_data->pt_dma_addr[src_page_nr] + page_offset;
	}

	return 0;
}
