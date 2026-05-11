/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SDXI device driver header
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#ifndef DMA_SDXI_H
#define DMA_SDXI_H

#include <linux/compiler_types.h>
#include <linux/dev_printk.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/types.h>

#include "mmio.h"

#define ID_TO_L1_INDEX(id)	((id) & 0x7F)

#define DESC_RING_BASE_PTR_SHIFT	6
#define CXT_STATUS_PTR_SHIFT		4
#define WRT_INDEX_PTR_SHIFT		3

#define L1_CXT_CTRL_PTR_SHIFT		6
#define L1_CXT_AKEY_PTR_SHIFT		12

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

	/* hardware capabilities (from cap0 & cap1) */
	u32 db_stride;			/* doorbell stride in bytes */
	u16 max_cxtid;			/* Maximum context ID allowed. */
	u32 op_grp_cap;			/* supported operation group cap */

	struct sdxi_cxt_L2_table *L2_table;
	dma_addr_t L2_dma;
	struct sdxi_cxt_L1_table *L1_table;
	dma_addr_t L1_dma;

	struct dma_pool *write_index_pool;
	struct dma_pool *cxt_sts_pool;
	struct dma_pool *cxt_ctl_pool;
	struct dma_pool *cst_blk_pool;

	struct sdxi_cxt *admin_cxt;

	const struct sdxi_bus_ops *bus_ops;
};

int sdxi_register(struct device *dev, const struct sdxi_bus_ops *ops);
void sdxi_unregister(struct device *dev);

static inline u64 sdxi_read64(const struct sdxi_dev *sdxi, enum sdxi_reg reg)
{
	return ioread64(sdxi->ctrl_regs + reg);
}

static inline void sdxi_write64(struct sdxi_dev *sdxi, enum sdxi_reg reg, u64 val)
{
	iowrite64(val, sdxi->ctrl_regs + reg);
}

#endif /* DMA_SDXI_H */
