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
#include <linux/types.h>

#define DEVICE_NAME		"aspeed-espi"

struct aspeed_espi {
	struct platform_device *pdev;
	struct device *dev;
	void __iomem *regs;
	struct reset_control *rst;
	struct clk *clk;
	int dev_id;
	int irq;
	const struct aspeed_espi_ops *ops;
};

#endif
