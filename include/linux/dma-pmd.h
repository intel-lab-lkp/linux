/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* See Documentation/core-api/dma-pmd.rst for the architecture overview. */
#ifndef _LINUX_DMA_PMD_H
#define _LINUX_DMA_PMD_H

#include <linux/types.h>
#include <linux/mm.h>
#include <linux/numa.h>
#include <linux/rcupdate.h>

struct device;
struct dma_pmd_pool;

#ifdef CONFIG_DMA_PMD

extern void *dma_pmd_meta_array;

bool __dma_is_pmd_page(unsigned long pfn);
bool __dma_is_pmd_direct(unsigned long pfn);

static inline bool dma_is_pmd_page(unsigned long pfn)
{
	/*
	 * Acquire pairs with smp_store_release() in dma_pmd_meta_init():
	 * orders both @dma_pmd_meta_nframes and the populated backing pages.
	 */
	if (likely(!smp_load_acquire(&dma_pmd_meta_array)))
		return false;

	return __dma_is_pmd_page(pfn);
}

static inline bool dma_is_pmd_direct(phys_addr_t phys)
{
	/* Pairs with smp_store_release() in dma_pmd_meta_init(). */
	if (likely(!smp_load_acquire(&dma_pmd_meta_array)))
		return false;

	return __dma_is_pmd_direct(phys >> PAGE_SHIFT);
}

bool __dma_pmd_free_page(struct page *page);

static inline bool dma_pmd_free_page(struct page *page)
{
	bool ret;

	rcu_read_lock();
	ret = dma_is_pmd_page(page_to_pfn(page)) && __dma_pmd_free_page(page);
	rcu_read_unlock();

	return ret;
}

/*
 * External API: dma_pmd_pool (streaming DMA page pool)
 * -----------------------------------------------------
 * Opportunistic drop-in for alloc_pages_node() that carves fixed-order
 * (order <= PMD_ORDER) pages out of 2MB physically contiguous buddy pages.
 *
 * - Creation (dma_pmd_pool_create):
 *   Records @order and @max_idle_pages; no 2MB pages or IOMMU mappings
 *   are allocated yet.
 *
 * - Allocation (dma_pmd_pool_alloc_node / dma_pmd_pool_alloc):
 *   Dispenses an order-@order compound page with refcount 1 (or NULL on
 *   unsupported GFP flags / OOM, so callers fall back to alloc_pages_node()).
 *   On a pool miss, allocates a 2MB page on @nid and splits it into subpages.
 *
 * - Mapping (dma_map_page / dma_map_single / dma_map_phys / dma_map_sg):
 *   Intercepts single-buffer and scatterlist maps. On first use of a 2MB page
 *   in an IOMMU domain, lazily installs a 2MB bidirectional leaf PTE in the
 *   domain's DMA_PMD IOVA window; subsequent maps are O(1) arithmetic.
 *
 * - Unmapping & Recycling (dma_unmap_* / put_page):
 *   dma_unmap_page/single/phys/sg() is a no-op for IOVAs in the DMA_PMD window,
 *   keeping the 2MB PTE resident across buffer reuse. When the page's last
 *   reference drops (put_page() / __free_pages()), __free_pages_prepare()
 *   intercepts it via dma_pmd_free_page() and recycles the subpage back into
 *   @pool.
 *
 * - Unmap & Buddy Release (reclaim / shrinker / dma_pmd_pool_destroy):
 *   A 2MB page is released back to the buddy allocator only when all of its
 *   subpages are free in @pool and either the pool exceeds @max_idle_pages, the
 *   system shrinker reclaims idle pages, or dma_pmd_pool_destroy() is called.
 *   Before __free_pages(PMD_ORDER) is called, the 2MB leaf PTE is unmapped from
 *   every domain that mapped it and the IOTLB is synchronously flushed.
 */
struct dma_pmd_pool *dma_pmd_pool_create(unsigned int order, unsigned int max_idle_pages);
struct dma_pmd_pool *dma_pmd_pool_destroy(struct dma_pmd_pool *pool);
unsigned long dma_pmd_pool_alloc_bulk_node(struct dma_pmd_pool *pool, gfp_t gfp,
					   int nid, unsigned long nr_pages,
					   struct page **page_array);
static inline struct page *dma_pmd_pool_alloc_node(struct dma_pmd_pool *pool,
						   gfp_t gfp, int nid)
{
	struct page *page = NULL;

	dma_pmd_pool_alloc_bulk_node(pool, gfp, nid, 1, &page);
	return page;
}
static inline struct page *dma_pmd_pool_alloc(struct dma_pmd_pool *pool, gfp_t gfp)
{
	return dma_pmd_pool_alloc_node(pool, gfp, NUMA_NO_NODE);
}
bool dma_pmd_pool_has_free(struct dma_pmd_pool *pool);

/* Hooks for kernel/dma/mapping.c */
void *dma_pmd_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
			gfp_t gfp, unsigned long attrs);
bool dma_pmd_arena_free(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma);
bool dma_is_pmd_dma(struct device *dev, dma_addr_t dma);

static inline bool dma_pmd_free(struct device *dev, size_t size,
				void *cpu_addr, dma_addr_t dma_handle)
{
	/* Pairs with smp_store_release() in dma_pmd_meta_init(). */
	if (likely(!smp_load_acquire(&dma_pmd_meta_array)))
		return false;
	return dma_pmd_arena_free(dev, size, cpu_addr, dma_handle);
}

#else /* !CONFIG_DMA_PMD */

static inline bool dma_is_pmd_page(unsigned long pfn)
{
	return false;
}

static inline bool dma_is_pmd_direct(phys_addr_t phys)
{
	return false;
}

static inline bool dma_pmd_free_page(struct page *page)
{
	return false;
}

static inline struct dma_pmd_pool *
dma_pmd_pool_create(unsigned int order, unsigned int max_idle_pages)
{
	return NULL;
}

static inline struct dma_pmd_pool *dma_pmd_pool_destroy(struct dma_pmd_pool *pool)
{
	return NULL;
}

static inline unsigned long
dma_pmd_pool_alloc_bulk_node(struct dma_pmd_pool *pool, gfp_t gfp, int nid,
			     unsigned long nr_pages, struct page **page_array)
{
	return 0;
}

static inline struct page *
dma_pmd_pool_alloc_node(struct dma_pmd_pool *pool, gfp_t gfp, int nid)
{
	return NULL;
}

static inline struct page *dma_pmd_pool_alloc(struct dma_pmd_pool *pool, gfp_t gfp)
{
	return NULL;
}

static inline bool dma_pmd_pool_has_free(struct dma_pmd_pool *pool)
{
	return false;
}

static inline void *dma_pmd_dma_alloc(struct device *dev, size_t size, dma_addr_t *dma,
				      gfp_t gfp, unsigned long attrs)
{
	return NULL;
}

static inline bool dma_pmd_arena_free(struct device *dev, size_t size,
				      void *cpu_addr, dma_addr_t dma)
{
	return false;
}

static inline bool dma_is_pmd_dma(struct device *dev, dma_addr_t dma)
{
	return false;
}

static inline bool dma_pmd_free(struct device *dev, size_t size,
				void *cpu_addr, dma_addr_t dma_handle)
{
	return false;
}

#endif /* CONFIG_DMA_PMD */
#endif /* _LINUX_DMA_PMD_H */
