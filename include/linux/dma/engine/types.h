/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Basic types shared by the DMA engine interfaces.
 */
#ifndef LINUX_DMA_ENGINE_TYPES_H
#define LINUX_DMA_ENGINE_TYPES_H

#include <linux/types.h>

/**
 * enum dma_slave_buswidth - defines bus width of the DMA slave
 * device, source or target buses
 * @DMA_SLAVE_BUSWIDTH_UNDEFINED: no bus width specified. In a
 *	&struct dma_slave_config it lets the controller pick a width, typically
 *	derived from the transfer itself. In a bus width mask it is a width a
 *	controller genuinely accepts, meaning it imposes no constraint of its
 *	own.
 * @DMA_SLAVE_BUSWIDTH_1_BYTE: 1 byte wide bus
 * @DMA_SLAVE_BUSWIDTH_2_BYTES: 2 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_3_BYTES: 3 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_4_BYTES: 4 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_8_BYTES: 8 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_16_BYTES: 16 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_32_BYTES: 32 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_64_BYTES: 64 bytes wide bus
 * @DMA_SLAVE_BUSWIDTH_128_BYTES: 128 bytes wide bus
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
};

#endif /* LINUX_DMA_ENGINE_TYPES_H */
