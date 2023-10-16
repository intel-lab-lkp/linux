// SPDX-License-Identifier: GPL-2.0
/*
 * Persistent-Across-Kexec memory (prmem) - Allocator.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

/* Page Allocation functions. */

static void prmem_expand(void)
{
	struct prmem_region	*region;
	struct page		*pages;
	unsigned int		order = MAX_ORDER;
	size_t			size = (1UL << order) << PAGE_SHIFT;

	if (prmem->cur_size + size > prmem->max_size)
		return;

	spin_unlock(&prmem_lock);
	pages = alloc_pages(GFP_NOWAIT, order);
	spin_lock(&prmem_lock);

	if (!pages)
		return;

	/* cur_size may have changed. Recheck. */
	if (prmem->cur_size + size > prmem->max_size)
		goto free;

	region = prmem_add_region(page_to_phys(pages), size);
	if (!region)
		goto free;

	pr_warn("%s: prmem expanded by %ld\n", __func__, size);
	return;
free:
	__free_pages(pages, order);
}

void *prmem_alloc_pages_locked(unsigned int order)
{
	struct prmem_region	*region;
	void			*va;
	size_t			size = (1UL << order) << PAGE_SHIFT;
	bool			expand = true;

retry:
	list_for_each_entry(region, &prmem->regions, node) {
		va = prmem_alloc_pool(region, size, size);
		if (va)
			return va;
	}
	if (expand) {
		expand = false;
		prmem_expand();
		goto retry;
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

/* Buffer allocation functions. */

#if PAGE_SIZE > 65536
#error "Page size is too big"
#endif

static size_t	prmem_cache_sizes[PRMEM_MAX_CACHES] = {
	8, 16, 32, 64, 128, 256, 512,
	1024, 2048, 4096, 8192, 16384, 32768, 65536,
};

static int prmem_cache_index(size_t size)
{
	int	i;

	for (i = 0; i < PRMEM_MAX_CACHES; i++) {
		if (size <= prmem_cache_sizes[i])
			return i;
	}
	BUG();
}

static void prmem_refill(void **cache, size_t size)
{
	void		*va;
	int		i, n = PAGE_SIZE / size;

	/* Allocate a page. */
	va = prmem_alloc_pages_locked(0);
	if (!va)
		return;

	/* Break up the page into pieces and put them in the cache. */
	for (i = 0; i < n; i++, va += size) {
		*((void **) va) = *cache;
		*cache = va;
	}
}

void *prmem_alloc_locked(size_t size)
{
	void		*va;
	int		index;
	void		**cache;

	index = prmem_cache_index(size);
	size = prmem_cache_sizes[index];

	cache = &prmem->caches[index];
	if (!*cache) {
		/* Refill the cache. */
		prmem_refill(cache, size);
	}

	/* Allocate one from the cache. */
	va = *cache;
	if (va)
		*cache = *((void **) va);
	return va;
}

void *prmem_alloc(size_t size, gfp_t gfp)
{
	void		*va;
	bool		zero = !!(gfp & __GFP_ZERO);

	if (!prmem_inited || !size)
		return NULL;

	/* This function is only for sizes up to a PAGE_SIZE. */
	if (size > PAGE_SIZE)
		return NULL;

	spin_lock(&prmem_lock);
	va = prmem_alloc_locked(size);
	spin_unlock(&prmem_lock);

	if (va && zero)
		memset(va, 0, size);
	return va;
}
EXPORT_SYMBOL_GPL(prmem_alloc);

void prmem_free_locked(void *va, size_t size)
{
	int		index;
	void		**cache;

	/* Free the object into its cache. */
	index = prmem_cache_index(size);
	cache = &prmem->caches[index];
	*((void **) va) = *cache;
	*cache = va;
}

void prmem_free(void *va, size_t size)
{
	if (!prmem_inited || !va || !size)
		return;

	/* This function is only for sizes up to a PAGE_SIZE. */
	if (size > PAGE_SIZE)
		return;

	spin_lock(&prmem_lock);
	prmem_free_locked(va, size);
	spin_unlock(&prmem_lock);
}
EXPORT_SYMBOL_GPL(prmem_free);
