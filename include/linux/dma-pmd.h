/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* See Documentation/core-api/dma-pmd.rst for the architecture overview. */
#ifndef _LINUX_DMA_PMD_H
#define _LINUX_DMA_PMD_H

#include <linux/compiler.h>
#include <linux/mm_types.h>
#include <linux/types.h>

#ifdef CONFIG_DMA_PMD

extern void *dma_pmd_meta_array;

bool __dma_is_pmd_page(unsigned long pfn);

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

#else /* !CONFIG_DMA_PMD */

static inline bool dma_is_pmd_page(unsigned long pfn)
{
	return false;
}

#endif /* CONFIG_DMA_PMD */
#endif /* _LINUX_DMA_PMD_H */
