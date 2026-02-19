/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Time Sensitive Networking (TSN) Ethernet MAC driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef XILINX_TSN_H
#define XILINX_TSN_H

#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define TSN_NUM_CLOCKS		6

#define TSN_DMA_CH_INVALID	GENMASK(7, 0)
#define TSN_DMA_MAX_TX_CH	GENMASK(3, 0)
#define TSN_MAX_TX_QUEUE	8
#define TSN_MIN_PRIORITIES	2
#define TSN_MAX_PRIORITIES	8
/**
 * struct tsn_priv - Main TSN private data structure
 * @pdev: Platform device handle
 * @dev: Device pointer for this TSN instance
 * @res: Platform resource information
 * @regs_start: Start address (physical) of mapped region
 * @regs: ioremap()'d base pointer
 * @clks: Bulk clock data for all required clocks
 * @tx_lock: Spinlock protecting TX rings and related TX state
 * @rx_lock: Spinlock protecting RX rings and related RX state
 * @mdio_lock: Mutex placeholder for future MDIO serialization
 * @num_priorities: Number of priority queues configured
 * @num_tx_queues: Number of TX DMA queues
 * @num_rx_queues: Number of RX DMA queues
 * @tx_dma_chan_map: Logical TX queue index to DMA channel number mapping.
 */
struct tsn_priv {
	struct platform_device *pdev;
	struct device *dev;
	struct resource *res;
	resource_size_t regs_start;
	void __iomem *regs;
	struct clk_bulk_data clks[TSN_NUM_CLOCKS];
	spinlock_t tx_lock;	/* Protects TX ring buffers */
	spinlock_t rx_lock;	/* Protects RX ring buffers */
	struct mutex mdio_lock; /* Serializes MDIO access across all EMACs */
	u32 num_priorities;
	u32 num_tx_queues;
	u32 num_rx_queues;
	u32 tx_dma_chan_map[TSN_MAX_TX_QUEUE];
};

#endif /* XILINX_TSN_H */
