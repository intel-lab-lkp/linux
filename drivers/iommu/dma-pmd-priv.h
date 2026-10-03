/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _DRIVERS_IOMMU_DMA_PMD_PRIV_H
#define _DRIVERS_IOMMU_DMA_PMD_PRIV_H

#include <linux/dma-pmd.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#ifdef CONFIG_DMA_PMD

/*
 * Normally 128 B per entry (64 KB per GB of RAM), or 256 B when spinlock
 * debugging enlarges struct dma_pmd_meta. Verified by static_assert().
 */
#if defined(CONFIG_DEBUG_SPINLOCK) || defined(CONFIG_DEBUG_LOCK_ALLOC)
#define DMA_PMD_META_SHIFT		8
#else
#define DMA_PMD_META_SHIFT		7
#endif
#define DMA_PMD_META_SIZE		BIT(DMA_PMD_META_SHIFT)

/**
 * struct dma_pmd_meta - Metadata for a single DMA_PMD page
 * @pooled: True while this page is owned by an dma_pmd_pool (at offset 0)
 *
 * Lives in the sparse per-PMD-frame array @dma_pmd_meta_array indexed by
 * (pfn >> PMD_ORDER). Only chunks covering valid RAM are backed by physical
 * pages (64 KB per GB of RAM).
 */
struct dma_pmd_meta {
	bool pooled;
} __aligned(DMA_PMD_META_SIZE);

static_assert(offsetof(struct dma_pmd_meta, pooled) == 0);
static_assert(sizeof(struct dma_pmd_meta) == DMA_PMD_META_SIZE);

extern unsigned long dma_pmd_meta_nframes;

static inline struct dma_pmd_meta *dma_pmd_meta_base(void)
{
	/*
	 * Acquire pairs with smp_store_release() in dma_pmd_meta_init():
	 * orders both @dma_pmd_meta_nframes and the populated backing pages.
	 */
	return smp_load_acquire(&dma_pmd_meta_array);
}

int dma_pmd_meta_init(void);
bool dma_pmd_meta_ensure_pfn(unsigned long pfn, bool can_block);
struct dma_pmd_meta *dma_pmd_meta_of_pfn(unsigned long pfn);
struct dma_pmd_meta *dma_pmd_meta_from_phys(phys_addr_t pa);
unsigned long dma_pmd_meta_to_pfn(const struct dma_pmd_meta *m);
phys_addr_t dma_pmd_meta_to_phys(const struct dma_pmd_meta *m);

#endif /* CONFIG_DMA_PMD */
#endif /* _DRIVERS_IOMMU_DMA_PMD_PRIV_H */
