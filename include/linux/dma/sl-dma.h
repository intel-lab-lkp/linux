/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Linux DMA Driver for the Smartlogic High Channel Count (HCC) DMA IP Core.
 *
 * Copyright (C) 2020 Pengutronix, Philipp Zabel <kernel@pengutronix.de>
 */
#ifndef _DMA_SL_DMA_H
#define _DMA_SL_DMA_H

#include <linux/types.h>

#define SL_DMA_MAX_WRITE_CHANNELS	64
#define SL_DMA_MAX_READ_CHANNELS	16
#define SL_DMA_MAX_INTERFACES		16

struct device;
struct dma_slave_map;

/**
 * struct sl_dma_write_channel_config - per-channel DMA Write configuration
 * channel: logical write channel number
 * interface: AXI Stream slave interface that writes to this channel
 * irq: design specific End of Frame global MSI-X IRQ number
 * page_size: destination buffer page size
 * reset: reset bit in the reset flag register, optional
 */
struct sl_dma_write_channel_config {
	u8 channel;
	u8 interface;
	u8 irq;
	u32 page_size;
	u32 reset;
};

/**
 * struct sl_dma_read_channel_config - per-channel DMA Read configuration
 * channel: logical read channel number
 * irq: design specific End of Request global MSI-X IRQ number
 * page_size: source buffer page size
 * image_format: bytes per transfer
 */
struct sl_dma_read_channel_config {
	u8 channel;
	u8 irq;
	u32 page_size;
	u32 image_format;
};

/**
 * struct sl_dma_write_channel_pair - pair channels that transfer together
 * @non_triggering_ch: the first channel to transfer, does not trigger interrupt
 * @triggering_ch: the second to transfer, triggers an interrupt
 */
struct sl_dma_write_channel_pair {
	u8 non_triggering_ch;
	u8 triggering_ch;
};

/**
 * struct sl_dma_config - design specific DMA IP Core configuration
 * @dma_map: array of struct dma_slave_map mappings
 * @dma_map_size: ARRAY_SIZE of dma_map
 * @num_write_channels: number of logical DMA write channels
 * @num_read_channels: number of logical DMA read channels
 * @num_write_channel_pairs: number of write channel pairs
 * @write_channels: DMA Write channel configuration array
 * @read_channels: DMA Read channel configuration array
 * @write_channel_pairs: array of paired write channels
 * @prepare_start: called before an AXI Stream slave interface is started
 * @reset_flag_reg: reset flag register offset. If non-zero, per-interface bits
 *                  assert during dmaengine_terminate_*() on write channels.
 * @user_interrupts: bitfield of interrupts issued by user logic.
 */
struct sl_dma_config {
	const struct dma_slave_map *dma_map;
	unsigned int dma_map_size;
	unsigned int num_write_channels;
	unsigned int num_read_channels;
	unsigned int num_write_channel_pairs;
	const struct sl_dma_write_channel_config *write_channels;
	const struct sl_dma_read_channel_config *read_channels;
	const struct sl_dma_write_channel_pair *write_channel_pairs;
	void (*prepare_start)(struct device *dev, unsigned int interface);
	unsigned int reset_flag_reg;
	u32 user_interrupts;
};

#endif /* _DMA_SL_DMA_H */
