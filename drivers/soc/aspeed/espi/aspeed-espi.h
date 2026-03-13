/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Unified eSPI driver header file and data structures
 * Copyright 2026 Aspeed Technology Inc.
 */
#ifndef ASPEED_ESPI_H
#define ASPEED_ESPI_H

#include <linux/irqreturn.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define DEVICE_NAME		"aspeed-espi"
#define ASPEED_ESPI_LUN_PATH_MAX	256

enum aspeed_tafs_mode {
	TAFS_MODE_SW = 1,
};

struct aspeed_espi_lun;

struct aspeed_espi_flash {
	struct {
		enum aspeed_tafs_mode mode;
		phys_addr_t taddr;
		resource_size_t size;
	} tafs;

	struct {
		bool enable;
		void *tx_virt;
		dma_addr_t tx_addr;
		void *rx_virt;
		dma_addr_t rx_addr;
	} dma;

	struct mutex tx_mtx; /* protects tx virt/addr */

	struct work_struct rx_work;

	struct mutex lun_mtx; /* protects lun metadata r/w */
	struct aspeed_espi_lun *lun;
	char lun_path[ASPEED_ESPI_LUN_PATH_MAX];
	bool lun_ro;
};

struct aspeed_espi {
	struct platform_device *pdev;
	struct device *dev;
	void __iomem *regs;
	struct reset_control *rst;
	struct clk *clk;
	int dev_id;
	int irq;
	struct aspeed_espi_flash flash;
	const struct aspeed_espi_ops *ops;
};

#endif
