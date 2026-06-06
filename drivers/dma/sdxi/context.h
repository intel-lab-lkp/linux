/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Advanced Micro Devices, Inc.
 */

#ifndef DMA_SDXI_CONTEXT_H
#define DMA_SDXI_CONTEXT_H

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/types.h>

#include "hw.h"
#include "sdxi.h"

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
	static_assert(sizeof(*tbl) == SZ_4K);
	return 0;
}

/* Submission Queue */
struct sdxi_sq {
	u32 ring_entries;
	u32 ring_size;
	struct sdxi_desc *desc_ring;
	dma_addr_t ring_dma;

	__le64 *write_index;
	dma_addr_t write_index_dma;

	struct sdxi_cxt_sts *cxt_sts;
	dma_addr_t cxt_sts_dma;
};

struct sdxi_cxt {
	struct sdxi_dev *sdxi;
	u16 id;

	__le64 __iomem *db;

	struct sdxi_cxt_ctl *cxt_ctl;
	dma_addr_t cxt_ctl_dma;

	struct sdxi_akey_table *akey_table;
	dma_addr_t akey_table_dma;

	struct sdxi_sq *sq;
};

int sdxi_admin_cxt_init(struct sdxi_dev *sdxi);

static inline void sdxi_cxt_push_doorbell(struct sdxi_cxt *cxt, u64 index)
{
	writeq(index, cxt->db);
}

#endif /* DMA_SDXI_CONTEXT_H */
