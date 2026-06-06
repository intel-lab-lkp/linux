/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SDXI device driver header
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#ifndef DMA_SDXI_H
#define DMA_SDXI_H

#include <linux/compiler_types.h>
#include <linux/types.h>

struct sdxi_dev;

/**
 * struct sdxi_bus_ops - Bus-specific methods for SDXI devices.
 */
struct sdxi_bus_ops {
	/**
	 * @init: Map control registers and doorbell region, allocate
	 *        IRQ ranges. Invoked before bus-agnostic SDXI
	 *        function initialization.
	 */
	int (*init)(struct sdxi_dev *sdxi);
};

struct sdxi_dev {
	struct device *dev;
	void __iomem *ctrl_regs;	/* virt addr of ctrl registers */
	void __iomem *dbs;		/* virt addr of doorbells */

	const struct sdxi_bus_ops *bus_ops;
};

int sdxi_register(struct device *dev, const struct sdxi_bus_ops *ops);

#endif /* DMA_SDXI_H */
