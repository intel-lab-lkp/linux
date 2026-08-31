/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Basic types shared by the DMA engine interfaces.
 */
#ifndef LINUX_DMA_ENGINE_TYPES_H
#define LINUX_DMA_ENGINE_TYPES_H

#include <linux/bitops.h>
#include <linux/types.h>

/**
 * enum dma_slave_buswidth - defines bus width of the DMA slave
 * device, source or target buses
 */
enum dma_slave_buswidth {
	DMA_SLAVE_BUSWIDTH_UNDEFINED = 0,
	DMA_SLAVE_BUSWIDTH_1_BYTE = 1,
	DMA_SLAVE_BUSWIDTH_2_BYTES = 2,
	DMA_SLAVE_BUSWIDTH_3_BYTES = 3,
	DMA_SLAVE_BUSWIDTH_4_BYTES = 4,
	DMA_SLAVE_BUSWIDTH_8_BYTES = 8,
	DMA_SLAVE_BUSWIDTH_16_BYTES = 16,
	DMA_SLAVE_BUSWIDTH_32_BYTES = 32,
	DMA_SLAVE_BUSWIDTH_64_BYTES = 64,
	DMA_SLAVE_BUSWIDTH_128_BYTES = 128,
	DMA_SLAVE_BUSWIDTH_MAX
};

/**
 * typedef dma_buswidth_mask_t - bus width capabilities bitmap modeled after
 * dma_cap_mask_t.
 *
 * Each supported bus width is represented by the bit whose position equals the
 * corresponding enum dma_slave_buswidth value, e.g. a device supporting a bus
 * width of 4 bytes has bit 4 set.
 */
typedef struct {
	DECLARE_BITMAP(bits, DMA_SLAVE_BUSWIDTH_MAX);
} dma_buswidth_mask_t;

#endif /* LINUX_DMA_ENGINE_TYPES_H */
