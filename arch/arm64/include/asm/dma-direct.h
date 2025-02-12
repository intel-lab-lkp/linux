/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_DMA_DIRECT_H
#define __ASM_DMA_DIRECT_H

#include <asm/pgtable-prot.h>

static inline unsigned long addr_to_shared(unsigned long addr)
{
	if (is_realm_world())
		addr |= prot_ns_shared;
	return addr;
}

static inline unsigned long addr_to_private(unsigned long addr)
{
	if (is_realm_world())
		addr &= prot_ns_shared - 1;
	return addr;
}

static inline dma_addr_t phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	return __phys_to_dma(dev, paddr);
}

static inline dma_addr_t phys_to_dma_unencrypted(struct device *dev,
						 phys_addr_t paddr)
{
	return addr_to_shared(__phys_to_dma(dev, paddr));
}
#define phys_to_dma_unencrypted phys_to_dma_unencrypted

static inline phys_addr_t dma_to_phys(struct device *dev, dma_addr_t dma_addr)
{
	return addr_to_private(__dma_to_phys(dev, dma_addr));
}

#endif	/* __ASM_DMA_DIRECT_H */
