// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * DMA_PMD coherent arena allocator (ARENA_REGION).
 *
 * See Documentation/core-api/dma-pmd.rst for the architecture overview.
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dma-pmd.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

#include "dma-iommu.h"
#include "dma-pmd-priv.h"
/*
 * dma_pmd_arena (ARENA_REGION allocator for coherent buffers)
 * -----------------------------------------------------------
 * Packs long-lived coherent DMA allocations into PMD physical pages mapped
 * via PMD IOMMU leaf PTEs in the 16GB ARENA_REGION at the bottom of each
 * domain's DMA_PMD IOVA window.
 *
 * - Allocation & Mapping (dma_pmd_arena_alloc / dma_pmd_dma_alloc):
 *   Allocates a PAGE_SIZE-aligned, zeroed region of @size bytes in
 *   ARENA_REGION, returning the CPU virtual address and storing the IOVA in
 *   *@dma. Each PMD arena page is mapped into a single IOMMU domain and is
 *   never shared across domains. Allocations <= 1MB first try partially used
 *   PMD arena pages already mapped in the caller's domain before allocating a
 *   fresh PMD page. Allocations > 1MB allocate contiguous empty PMD slots in
 *   ARENA_REGION and vmap() them when spanning multiple PMD pages; if the
 *   trailing PMD page has leftover 4KB blocks, it becomes available for
 *   subsequent <= 1MB allocations in the same domain.
 *
 * - Unmapping & Release (dma_free_coherent / dma_pmd_free):
 *   dma_pmd_free() checks whether @dma_handle falls in @dev's ARENA_REGION and,
 *   if so, unreserves the 4KB blocks via dma_pmd_arena_free(). Once all 512 4KB
 *   blocks of a PMD slot are free, its PMD PTE is unmapped from its domain and
 *   the backing PMD page is returned to the buddy allocator.
 */

struct dma_pmd_arena {
	spinlock_t lock;	/* protects bitmaps and arena metadata */
	DECLARE_BITMAP(fully_empty, DMA_PMD_ARENA_PAGES);
	DECLARE_BITMAP(partial_empty, DMA_PMD_ARENA_PAGES);
};

/* Currently one instance for all allocations */
static struct dma_pmd_arena dma_pmd_arena __cacheline_aligned_in_smp = {
	.lock = __SPIN_LOCK_UNLOCKED(dma_pmd_arena.lock),
	.fully_empty = { [0 ... BITS_TO_LONGS(DMA_PMD_ARENA_PAGES) - 1] = ~0UL },
};

/* Provide a contiguous VA range for multi-page allocations */
static void *dma_pmd_arena_vmap(unsigned int slot, unsigned int npages)
{
	unsigned int nr_subpages = npages * DMA_PMD_BLOCKS(0);
	unsigned int subpages_per_2m = DMA_PMD_BLOCKS(0);
	struct page *page, **pages;
	unsigned int i, j;
	void *va;

	pages = kvmalloc_array(nr_subpages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;

	for (i = 0; i < npages; i++) {
		page = dma_pmd_arena_meta(slot + i)->arena_page;
		for (j = 0; j < subpages_per_2m; j++)
			pages[i * subpages_per_2m + j] = page + j;
	}

	va = dma_common_pages_remap(pages, nr_subpages << PAGE_SHIFT,
				    PAGE_KERNEL, __builtin_return_address(0));
	if (!va)
		kvfree(pages);
	return va;
}

/* Provide a contiguous IOVA range for allocations */
static int dma_pmd_arena_map_domain(struct device *dev, struct iommu_domain *domain,
				    struct dma_pmd_window *win, struct dma_pmd_meta *m)
{
	unsigned int domain_idx;
	unsigned long flags;
	dma_addr_t iova;
	int prot, ret;
	u64 limit;

	if (!domain)
		return 0;

	iova = win->base + dma_pmd_meta_win_offset(m);
	limit = min_not_zero(dma_get_mask(dev), dev->bus_dma_limit);
	if (iova + PMD_SIZE - 1 > limit)
		return -ENOSPC;

	domain_idx = win->domain_idx - DMA_PMD_IDX_FIRST;
	if (test_bit(domain_idx, &m->domains_mapped))
		return 0;

	prot = dma_info_to_prot(DMA_BIDIRECTIONAL, true, 0);
	spin_lock_irqsave(&m->map_lock, flags);
	if (test_bit(domain_idx, &m->domains_mapped)) {
		ret = 0;
	} else if (unlikely(!dma_pmd_domain_active(domain_idx, domain))) {
		ret = -ENODEV;
	} else {
		ret = iommu_map(domain, iova, page_to_phys(m->arena_page),
				PMD_SIZE, prot, GFP_ATOMIC);
		if (!ret) {
			/* Ensure the leaf PTE is visible before setting the bit. */
			smp_mb__before_atomic();
			set_bit(domain_idx, &m->domains_mapped);
		}
	}
	spin_unlock_irqrestore(&m->map_lock, flags);
	return ret;
}

/**
 * dma_pmd_arena_free - Unreserve blocks in an ARENA_REGION allocation
 * @dev: Device passed to dma_free_coherent()
 * @size: Size of the allocation being freed
 * @cpu_addr: CPU virtual address passed to dma_free_coherent()
 * @dma: DMA address passed to dma_free_coherent()
 *
 * Return: true if @dma belonged to ARENA_REGION and was freed.
 */
bool dma_pmd_arena_free(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma)
{
	unsigned int slot, off, nblocks, bpp = DMA_PMD_BLOCKS(0);
	struct iommu_domain *domain;
	struct dma_pmd_window *win;
	dma_addr_t arena_base;
	unsigned long flags;

	if (!dev || !cpu_addr)
		return false;

	if (!dma_pmd_meta_base())
		return false;

	if (dev->iommu_group) {
		domain = iommu_get_dma_domain(dev);
		win = domain ? dma_pmd_dma_window(domain) : NULL;
		/* Pairs with smp_store_release() in dma_pmd_window_assign(). */
		if (!win || !smp_load_acquire(&win->size))
			return false;
		arena_base = win->base;
	} else if (IS_ENABLED(CONFIG_DMA_PMD_META_KUNIT_TEST)) {
		/* Reachable only from KUnit tests with a dummy device. */
		arena_base = 0;
	} else {
		return false;
	}

	if (dma < arena_base || dma - arena_base >= ARENA_REGION_SIZE)
		return false;

	if (is_vmalloc_addr(cpu_addr)) {
		kvfree(dma_common_find_pages(cpu_addr));
		dma_common_free_remap(cpu_addr, size);
	}

	slot = (dma - arena_base) >> PMD_SHIFT;
	off = ((dma - arena_base) & (PMD_SIZE - 1)) >> PAGE_SHIFT;
	nblocks = ALIGN(size, PAGE_SIZE) >> PAGE_SHIFT;

	while (nblocks > 0 && slot < DMA_PMD_ARENA_PAGES) {
		struct dma_pmd_meta *m = dma_pmd_arena_meta(slot);
		unsigned int n = min(nblocks, bpp - off);
		struct page *free_page = NULL;

		spin_lock_irqsave(&dma_pmd_arena.lock, flags);
		if (!WARN_ON_ONCE(!m->arena_page)) {
			bitmap_clear(m->free_bitmap, off, n);
			m->nr_free += n;
			if (m->nr_free == bpp) {
				free_page = m->arena_page;
				m->arena_page = NULL;
				m->nr_free = 0;
				__clear_bit(slot, dma_pmd_arena.partial_empty);
			} else {
				__set_bit(slot, dma_pmd_arena.partial_empty);
			}
		}
		spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);

		if (free_page) {
			dma_pmd_unmap_all(m);
			__free_pages(free_page, PMD_ORDER);
			spin_lock_irqsave(&dma_pmd_arena.lock, flags);
			__set_bit(slot, dma_pmd_arena.fully_empty);
			spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);
		}

		nblocks -= n;
		off = 0;
		slot++;
	}

	return true;
}
EXPORT_SYMBOL(dma_pmd_arena_free);

/**
 * dma_pmd_arena_alloc - Carve a DMA-mapped buffer out of ARENA_REGION
 * @dev: Device the buffer is allocated and mapped for
 * @size: Requested size, rounded up to 4KB
 * @dma: Out parameter, DMA address of the returned buffer in ARENA_REGION
 * @node: Preferred NUMA node, or NUMA_NO_NODE for the device's own node
 *
 * Locking: Acquires @dma_pmd_arena.lock (irqsave) when searching or updating
 *          slot bitmaps.
 *
 * Return: Kernel virtual address of the zeroed buffer, or NULL if the caller
 *         should fall back to its own allocation (e.g. dma_alloc_coherent()).
 */
void *dma_pmd_arena_alloc(struct device *dev, size_t size, dma_addr_t *dma, int node)
{
	unsigned int npages, nblocks, slot, i, bpp = DMA_PMD_BLOCKS(0);
	unsigned long *bitmap = dma_pmd_arena.fully_empty;
	struct iommu_domain *domain;
	struct dma_pmd_window *win;
	dma_addr_t arena_base;
	unsigned long flags;
	void *va;

	if (!dev || (!dev->iommu_group && !IS_ENABLED(CONFIG_DMA_PMD_META_KUNIT_TEST)))
		return NULL;

	if (node == NUMA_NO_NODE)
		node = dev_to_node(dev);

	size = ALIGN(size, PAGE_SIZE);
	if (!size || size > ARENA_REGION_SIZE)
		return NULL;

	domain = dev->iommu_group ? iommu_get_dma_domain(dev) : NULL;
	win = domain ? dma_pmd_dma_window(domain) : NULL;
	if (dev->iommu_group && (!win || !(domain->pgsize_bitmap & PMD_SIZE)))
		goto err_fallback;

	if (unlikely(dma_pmd_meta_init()))
		goto err_fallback;

	/* Pairs with smp_store_release() in dma_pmd_window_assign(). */
	if (win && !smp_load_acquire(&win->size) &&
	    (READ_ONCE(win->domain_idx) != DMA_PMD_IDX_NONE ||
	     dma_pmd_window_assign(dev, domain, win)))
		goto err_fallback;

	arena_base = win ? win->base : 0;
	nblocks = size >> PAGE_SHIFT;

	/*
	 * Allocations <= 1MB first try partially empty pages already mapped in
	 * @domain (arena PMD pages are single-domain and never shared across
	 * IOMMU domains).
	 */
	if (size <= SZ_1M) {
		unsigned long domain_mask = win ? BIT(win->domain_idx - DMA_PMD_IDX_FIRST) : 0;
		u64 limit = min_not_zero(dma_get_mask(dev), dev->bus_dma_limit);

		spin_lock_irqsave(&dma_pmd_arena.lock, flags);
		for_each_set_bit(slot, dma_pmd_arena.partial_empty, DMA_PMD_ARENA_PAGES) {
			struct dma_pmd_meta *m = dma_pmd_arena_meta(slot);
			unsigned long off;

			if (READ_ONCE(m->domains_mapped) != domain_mask ||
			    (domain &&
			     arena_base + ((dma_addr_t)(slot + 1) << PMD_SHIFT) - 1 > limit))
				continue;
			if (m->nr_free < nblocks)
				continue;

			off = bitmap_find_next_zero_area(m->free_bitmap, bpp, 0, nblocks,
							 (1UL << get_order(size)) - 1);
			if (off >= bpp)
				continue;

			bitmap_set(m->free_bitmap, off, nblocks);
			m->nr_free -= nblocks;
			if (m->nr_free == 0)
				__clear_bit(slot, dma_pmd_arena.partial_empty);
			va = page_address(m->arena_page) + (off << PAGE_SHIFT);
			*dma = arena_base + ((dma_addr_t)slot << PMD_SHIFT) +
			       (off << PAGE_SHIFT);
			spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);

			memset(va, 0, size);
			return va;
		}
		spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);
	}

	/*
	 * Allocations > 1MB (or <= 1MB when no partial page fits) look for
	 * @npages contiguous fully empty pages.
	 */
	npages = DIV_ROUND_UP(size, PMD_SIZE);
	spin_lock_irqsave(&dma_pmd_arena.lock, flags);
	slot = 0;
	while (slot + npages <= DMA_PMD_ARENA_PAGES) {
		unsigned int next_zero;

		slot = find_next_bit(bitmap, DMA_PMD_ARENA_PAGES, slot);
		if (slot + npages > DMA_PMD_ARENA_PAGES)
			break;
		next_zero = find_next_zero_bit(bitmap, slot + npages, slot);
		if (next_zero >= slot + npages) {
			bitmap_clear(bitmap, slot, npages);
			break;
		}
		slot = next_zero + 1;
	}
	spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);

	if (slot + npages > DMA_PMD_ARENA_PAGES)
		goto err_fallback;

	for (i = 0; i < npages; i++) {
		struct dma_pmd_meta *m = dma_pmd_arena_meta(slot + i);
		gfp_t gfp = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN;
		struct page *page;

		page = (node == NUMA_NO_NODE) ? alloc_pages(gfp, PMD_ORDER) :
			alloc_pages_node(node, gfp, PMD_ORDER);
		if (!page)
			goto err_free_pages;

		m->arena_page = page;
		if (dma_pmd_arena_map_domain(dev, domain, win, m)) {
			m->arena_page = NULL;
			__free_pages(page, PMD_ORDER);
			goto err_free_pages;
		}
	}

	va = (npages == 1) ? page_address(dma_pmd_arena_meta(slot)->arena_page) :
			     dma_pmd_arena_vmap(slot, npages);
	if (!va)
		goto err_free_pages;

	spin_lock_irqsave(&dma_pmd_arena.lock, flags);
	for (i = 0; i < npages; i++) {
		struct dma_pmd_meta *m = dma_pmd_arena_meta(slot + i);
		unsigned int n = min(nblocks - i * bpp, bpp);

		bitmap_zero(m->free_bitmap, bpp);
		bitmap_set(m->free_bitmap, 0, n);
		m->nr_free = bpp - n;
		if (m->nr_free > 0)
			__set_bit(slot + i, dma_pmd_arena.partial_empty);
	}
	spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);

	*dma = arena_base + ((dma_addr_t)slot << PMD_SHIFT);
	return va;

err_free_pages:
	while (i--) {
		struct dma_pmd_meta *m = dma_pmd_arena_meta(slot + i);
		struct page *page = m->arena_page;

		m->arena_page = NULL;
		dma_pmd_unmap_all(m);
		__free_pages(page, PMD_ORDER);
	}
	spin_lock_irqsave(&dma_pmd_arena.lock, flags);
	bitmap_set(bitmap, slot, npages);
	spin_unlock_irqrestore(&dma_pmd_arena.lock, flags);
err_fallback:
	return NULL;
}
EXPORT_SYMBOL(dma_pmd_arena_alloc);

void *dma_pmd_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
			gfp_t gfp, unsigned long attrs)
{
	void *va;

	if (!dev || !dev->iommu_group || !dev_is_dma_coherent(dev))
		return NULL;
	/*
	 * The arena allocates PMD pages, IOMMU page tables, and vmap() areas
	 * with GFP_KERNEL, and shares PMD pages across allocations on the same
	 * device. Decline non-GFP_KERNEL requests (including GFP_NOFS/GFP_NOIO
	 * or scoped memalloc_nofs/noio contexts) and per-allocation modifiers
	 * such as __GFP_ACCOUNT or __GFP_NORETRY so the caller falls back to
	 * the standard DMA allocator.
	 */
	if ((current_gfp_context(gfp) & ~(__GFP_ZERO | __GFP_NOWARN)) != GFP_KERNEL ||
	    (attrs & ~DMA_ATTR_NO_WARN))
		return NULL;

	va = dma_pmd_arena_alloc(dev, size, dma, dev_to_node(dev));
	if (!va && !(gfp & __GFP_NOWARN) && !(attrs & DMA_ATTR_NO_WARN))
		dev_warn_ratelimited(dev, "DMA_PMD arena alloc failed (size %zu)\n",
				     size);
	return va;
}
EXPORT_SYMBOL(dma_pmd_dma_alloc);
