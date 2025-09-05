/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SDXI device driver header
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __SDXI_H
#define __SDXI_H

#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/dmaengine.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "../virt-dma.h"
#include "hw.h"
#include "mmio.h"

#define SDXI_DRV_NAME		"sdxi"

#define ID_TO_L2_INDEX(id)	(((id) >> 9) & 0x1FF)
#define ID_TO_L1_INDEX(id)	((id) & 0x7F)
#define IS_VF_DEVICE(sdxi)	((sdxi)->is_vf)

#define L2_TABLE_ENTRIES	(1 << 9)
#define L1_TABLE_ENTRIES	(1 << 7)
#define L2_TABLE_SIZE		4096
#define L1_TABLE_SIZE		4096

#define OP_TYPE_ERRLOG          0x7f7

#define DESC_RING_BASE_PTR_SHIFT	6
#define CXT_STATUS_PTR_SHIFT		4
#define WRT_INDEX_PTR_SHIFT		3

#define L1_CXT_CTRL_PTR_SHIFT		6
#define L1_CXT_AKEY_PTR_SHIFT		12

#define MAX_DMA_COPY_BYTES		(1ULL << 32)

/* Submission Queue */
struct sdxi_sq {
	struct sdxi_cxt *cxt;		/* owner */

	u32 ring_entries;
	u32 ring_size;
	struct sdxi_desc *desc_ring;
	dma_addr_t ring_dma;

	__le64 *write_index;
	dma_addr_t write_index_dma;

	struct sdxi_cxt_sts *cxt_sts;
	dma_addr_t cxt_sts_dma;

	/* NB: define doorbell here */
};

struct sdxi_tasklet_data {
	struct sdxi_cmd *cmd;
};

struct sdxi_cmd {
	struct work_struct work;
	struct sdxi_cxt *cxt;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
	int ret;
	size_t len;
	u64 src_addr;
	u64 dst_addr;
	/* completion callback support */
	void (*sdxi_cmd_callback)(void *data, int err);
	void *data;
};

struct sdxi_dma_chan {
	struct virt_dma_chan vc;
	struct sdxi_cxt *cxt;
};

/*
 * The size of the AKey table is flexible, from 4KB to 1MB. Always use
 * the minimum size for now.
 */
struct sdxi_akey_table {
	struct sdxi_akey_ent entry[SZ_4K / sizeof(struct sdxi_akey_ent)];
};

/* For encoding the akey table size in CXT_L1_ENT's akey_sz. */
static inline u8 akey_table_order(const struct sdxi_akey_table *tbl)
{
	static_assert(sizeof(struct sdxi_akey_table) == SZ_4K);
	return 0;
}

/* Context */
struct sdxi_cxt {
	struct sdxi_dev *sdxi;	/* owner */
	unsigned int id;

	resource_size_t db_base;	/* doorbell MMIO base addr */
	__le64 __iomem *db;		/* doorbell virt addr */

	struct sdxi_cxt_ctl *cxt_ctl;
	dma_addr_t cxt_ctl_dma;

	struct sdxi_akey_table *akey_table;
	dma_addr_t akey_table_dma;

	struct sdxi_sq *sq;

	/* NB: might need to move to sdxi_device? */
	struct sdxi_dma_chan sdxi_dma_chan;

	struct sdxi_process *process;	/* process reprsentation */
};

/**
 * struct sdxi_dev_ops - Bus-specific methods for SDXI devices.
 *
 * @irq_init: Allocate MSIs.
 * @irq_exit: Release MSIs.
 */
struct sdxi_dev_ops {
	int (*irq_init)(struct sdxi_dev *sdxi);
	void (*irq_exit)(struct sdxi_dev *sdxi);
};

struct sdxi_dev {
	struct device *dev;
	resource_size_t ctrl_regs_bar;	/* ctrl registers base (BAR0) */
	resource_size_t dbs_bar;	/* doorbells base (BAR2) */
	void __iomem *ctrl_regs;	/* virt addr of ctrl registers */
	void __iomem *dbs;		/* virt addr of doorbells */

	/* hardware capabilities (from cap0 & cap1) */
	u16 sfunc;			/* function's requester id */
	u32 db_stride;			/* doorbell stride in bytes */
	u64 max_ring_entries;		/* max # of ring entries supported */

	u32 max_akeys;			/* max akey # supported */
	u32 max_cxts;			/* max contexts # supported */
	u32 op_grp_cap;			/* supported operatation group cap */

	/* context management */
	struct mutex cxt_lock;		/* context protection */
	int cxt_count;
	struct sdxi_cxt_l2_table *l2_table;
	dma_addr_t l2_dma;
	/* list of context l1 tables, on-demand, access with [l2_idx] */
	struct sdxi_cxt_l1_table *l1_table_array[L2_TABLE_ENTRIES];
	/* all contexts, on-demand, access with [l2_idx][l1_idx] */
	struct sdxi_cxt **cxt_array[L2_TABLE_ENTRIES];

	struct dma_pool *write_index_pool;
	struct dma_pool *cxt_sts_pool;
	struct dma_pool *cxt_ctl_pool;

	/* error log */
	int error_irq;
	struct sdxi_errlog_hd_ent *err_log;
	dma_addr_t err_log_dma;

	/* DMA engine */
	struct dma_device dma_dev;
	struct sdxi_dma_chan *sdxi_dma_chan;
	struct sdxi_tasklet_data tdata;

	/* special contexts */
	struct sdxi_cxt *admin_cxt;	/* admin context */
	struct sdxi_cxt *dma_cxt;	/* DMA engine context */

	const struct sdxi_dev_ops *dev_ops;
};

static inline struct device *sdxi_to_dev(const struct sdxi_dev *sdxi)
{
	return sdxi->dev;
}

#define sdxi_dbg(s, fmt, ...) dev_dbg(sdxi_to_dev(s), fmt, ## __VA_ARGS__)
#define sdxi_info(s, fmt, ...) dev_info(sdxi_to_dev(s), fmt, ## __VA_ARGS__)
#define sdxi_err(s, fmt, ...) dev_err(sdxi_to_dev(s), fmt, ## __VA_ARGS__)

/* Device Control */
int sdxi_device_init(struct sdxi_dev *sdxi, const struct sdxi_dev_ops *ops);
void sdxi_device_exit(struct sdxi_dev *sdxi);

static inline u64 sdxi_read64(const struct sdxi_dev *sdxi, enum sdxi_reg reg)
{
	return ioread64(sdxi->ctrl_regs + reg);
}

static inline void sdxi_write64(struct sdxi_dev *sdxi, enum sdxi_reg reg, u64 val)
{
	iowrite64(val, sdxi->ctrl_regs + reg);
}

#endif /* __SDXI_H */
