/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_KHO_ABI_DMA_ALLOC_H
#define _LINUX_KHO_ABI_DMA_ALLOC_H

#include <linux/types.h>

/**
 * DOC: DMA Alloc ABI
 *
 * This header defines the structures used to serialize the state of DMA
 * allocations, done by device driver, across a Live Update.
 *
 * Only DMA allocations done through dma-direct that are contiguous and
 * allocated using alloc_page are supported.
 */

/**
 * struct dma_alloc_ser - Serialized state of a single DMA allocation
 * @page_phys: Physical address of the preserved pages
 * @size: Size of the DMA allocation
 * @attrs: DMA allocation attributes
 * @force_decrypted: Whether the memory is force decrypted in previous kernel
 */
struct dma_alloc_ser {
	u64 page_phys;
	u64 size;
	u64 attrs;
	u8 force_decrypted;
	u8 padding[7];
} __packed;

#endif /* _LINUX_KHO_ABI_DMA_ALLOC_H */
