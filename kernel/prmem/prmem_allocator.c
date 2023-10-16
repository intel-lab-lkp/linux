// SPDX-License-Identifier: GPL-2.0
/*
 * Persistent-Across-Kexec memory feature (prmem) - Allocator.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

/* Page Allocation functions. */

void *prmem_alloc_pages_locked(unsigned int order)
{
	struct prmem_region	*region;
	void			*va;
	size_t			size = (1UL << order) << PAGE_SHIFT;

	list_for_each_entry(region, &prmem->regions, node) {
		va = prmem_alloc_pool(region, size, size);
		if (va)
			return va;
	}
	return NULL;
}

struct page *prmem_alloc_pages(unsigned int order, gfp_t gfp)
{
	void		*va;
	size_t		size = (1UL << order) << PAGE_SHIFT;
	bool		zero = !!(gfp & __GFP_ZERO);

	if (!prmem_inited || order > MAX_ORDER)
		return NULL;

	spin_lock(&prmem_lock);
	va = prmem_alloc_pages_locked(order);
	spin_unlock(&prmem_lock);

	if (va) {
		if (zero)
			memset(va, 0, size);
		return virt_to_page(va);
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(prmem_alloc_pages);

void prmem_free_pages_locked(void *va, unsigned int order)
{
	struct prmem_region	*region;
	size_t			size = (1UL << order) << PAGE_SHIFT;
	void			*eva = va + size;
	void			*region_va;

	list_for_each_entry(region, &prmem->regions, node) {
		/* The region structure is at the base of the region memory. */
		region_va = region;
		if (va >= region_va && eva <= (region_va + region->size)) {
			prmem_free_pool(region, va, size);
			return;
		}
	}
}

void prmem_free_pages(struct page *pages, unsigned int order)
{
	if (!prmem_inited || order > MAX_ORDER)
		return;

	spin_lock(&prmem_lock);
	prmem_free_pages_locked(page_to_virt(pages), order);
	spin_unlock(&prmem_lock);
}
EXPORT_SYMBOL_GPL(prmem_free_pages);
