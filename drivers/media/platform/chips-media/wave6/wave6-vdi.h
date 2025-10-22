/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - low level access interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#ifndef __WAVE6_VDI_H__
#define __WAVE6_VDI_H__

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <media/videobuf2-dma-contig.h>
#include "wave6-vpuconfig.h"

enum endian_mode {
	VDI_128BIT_BIG_ENDIAN = 0,
	VDI_128BIT_BE_BYTE_SWAP,
	VDI_128BIT_BE_WORD_SWAP,
	VDI_128BIT_BE_WORD_BYTE_SWAP,
	VDI_128BIT_BE_DWORD_SWAP,
	VDI_128BIT_BE_DWORD_BYTE_SWAP,
	VDI_128BIT_BE_DWORD_WORD_SWAP,
	VDI_128BIT_BE_DWORD_WORD_BYTE_SWAP,
	VDI_128BIT_LE_DWORD_WORD_BYTE_SWAP,
	VDI_128BIT_LE_DWORD_WORD_SWAP,
	VDI_128BIT_LE_DWORD_BYTE_SWAP,
	VDI_128BIT_LE_DWORD_SWAP,
	VDI_128BIT_LE_WORD_BYTE_SWAP,
	VDI_128BIT_LE_WORD_SWAP,
	VDI_128BIT_LE_BYTE_SWAP,
	VDI_128BIT_LITTLE_ENDIAN = 15,
	VDI_ENDIAN_MAX
};

/**
 * struct vpu_buf - VPU buffer for a coherent DMA buffer
 * @size:	Buffer size
 * @daddr:	Mapped address for device access
 * @vaddr:	Kernel virtual address
 * @dev:	Device pointer for DMA API
 *
 * Represents a buffer allocated via dma_alloc_coherent().
 */
struct vpu_buf {
	size_t size;
	dma_addr_t daddr;
	void *vaddr;
	struct device *dev;
};

static inline void wave6_vdi_writel(void __iomem *base, u32 addr, u32 data)
{
	writel(data, base + addr);
}

static inline unsigned int wave6_vdi_readl(void __iomem *base, u32 addr)
{
	return readl(base + addr);
}

static inline int wave6_vdi_alloc_dma(struct device *dev, struct vpu_buf *vb)
{
	void *vaddr;
	dma_addr_t daddr;

	if (!vb || !vb->size)
		return -EINVAL;

	vaddr = dma_alloc_coherent(dev, vb->size, &daddr, GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;

	vb->vaddr = vaddr;
	vb->daddr = daddr;
	vb->dev = dev;

	return 0;
}

static inline void wave6_vdi_free_dma(struct vpu_buf *vb)
{
	if (!vb || !vb->size || !vb->vaddr)
		return;

	dma_free_coherent(vb->dev, vb->size, vb->vaddr, vb->daddr);

	memset(vb, 0, sizeof(*vb));
}

#endif /* __WAVE6_VDI_H__ */
