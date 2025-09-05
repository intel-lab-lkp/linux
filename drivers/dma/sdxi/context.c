// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI submission queue (sq) and descriptor management
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt)     "SDXI: " fmt

#include <linux/delay.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/types.h>
#include <linux/wordpart.h>

#include "context.h"
#include "descriptor.h"
#include "enqueue.h"
#include "hw.h"
#include "sdxi.h"

/* Alloc sdxi_sq in kernel space */
static struct sdxi_sq *sdxi_sq_alloc(struct sdxi_cxt *cxt, int ring_entries)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct device *dev = sdxi_to_dev(sdxi);
	u64 write_index_ptr;
	struct sdxi_sq *sq;
	u64 ds_ring_ptr;
	u64 cxt_sts_ptr;
	u32 ds_ring_sz;

	/* alloc desc_ring */
	if (ring_entries > sdxi->max_ring_entries) {
		sdxi_err(sdxi, "%d ring entries requested, max is %llu\n",
			ring_entries, sdxi->max_ring_entries);
		return NULL;
	}

	sq = kzalloc(sizeof(*sq), GFP_KERNEL);
	if (!sq)
		return NULL;

	sq->ring_entries = ring_entries;
	sq->ring_size = sizeof(sq->desc_ring[0]) * sq->ring_entries;
	sq->desc_ring = dma_alloc_coherent(dev, sq->ring_size, &sq->ring_dma,
					   GFP_KERNEL);
	if (!sq->desc_ring)
		goto free_sq;

	sq->cxt_sts = dma_pool_zalloc(sdxi->cxt_sts_pool, GFP_KERNEL, &sq->cxt_sts_dma);
	if (!sq->cxt_sts)
		goto free_desc_ring;

	sq->write_index = dma_pool_zalloc(sdxi->write_index_pool, GFP_KERNEL,
					  &sq->write_index_dma);
	if (!sq->write_index)
		goto free_cxt_sts;

	/* final setup */
	if (cxt->id == SDXI_ADMIN_CXT_ID || cxt->id == SDXI_DMA_CXT_ID)
		sq->cxt_sts->state = FIELD_PREP(SDXI_CXT_STS_STATE, CXTV_RUN);

	write_index_ptr = FIELD_PREP(SDXI_CXT_CTL_WRITE_INDEX_PTR,
				     sq->write_index_dma >> 3);
	cxt_sts_ptr = FIELD_PREP(SDXI_CXT_CTL_CXT_STS_PTR,
				 sq->cxt_sts_dma >> 4);
	ds_ring_sz = sq->ring_size >> 6;

	cxt->cxt_ctl->write_index_ptr = cpu_to_le64(write_index_ptr);
	cxt->cxt_ctl->cxt_sts_ptr     = cpu_to_le64(cxt_sts_ptr);
	cxt->cxt_ctl->ds_ring_sz      = cpu_to_le32(ds_ring_sz);

	/* turn it on now */
	sq->cxt = cxt;
	cxt->sq = sq;
	ds_ring_ptr = (FIELD_PREP(SDXI_CXT_CTL_DS_RING_PTR, sq->ring_dma >> 6) |
		       FIELD_PREP(SDXI_CXT_CTL_VL, 1));
	dma_wmb();
	WRITE_ONCE(cxt->cxt_ctl->ds_ring_ptr, cpu_to_le64(ds_ring_ptr));

	sdxi_dbg(sdxi, "sq created, id=%d, cxt_ctl=%p\n"
		 "  desc ring addr:   v=0x%p:d=%pad\n"
		 "  write index addr: v=0x%p:d=%pad\n"
		 "  cxt status addr: v=0x%p:d=%pad\n",
		 cxt->id, cxt->cxt_ctl,
		 sq->desc_ring, &sq->ring_dma,
		 sq->write_index, &sq->write_index_dma,
		 sq->cxt_sts, &sq->cxt_sts_dma);

	return sq;

free_cxt_sts:
	dma_pool_free(sdxi->cxt_sts_pool, sq->cxt_sts, sq->cxt_sts_dma);
free_desc_ring:
	dma_free_coherent(dev, sq->ring_size, sq->desc_ring, sq->ring_dma);
free_sq:
	kfree(sq);
	return NULL;
}

static void sdxi_sq_free(struct sdxi_sq *sq)
{
	struct sdxi_cxt *cxt = sq->cxt;
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct device *dev = sdxi_to_dev(sdxi);

	if (!cxt)
		return;

	dma_pool_free(sdxi->write_index_pool, sq->write_index, sq->write_index_dma);
	dma_pool_free(sdxi->cxt_sts_pool, sq->cxt_sts, sq->cxt_sts_dma);
	dma_free_coherent(dev, sq->ring_size, sq->desc_ring, sq->ring_dma);

	cxt->sq = NULL;
	kfree(sq);
}

/* Default size 1024 ==> 64KB descriptor ring, guaranteed */
#define DEFAULT_DESC_RING_ENTRIES	1024
static struct sdxi_sq *sdxi_sq_alloc_default(struct sdxi_cxt *cxt)
{
	return sdxi_sq_alloc(cxt, DEFAULT_DESC_RING_ENTRIES);
}

static bool sdxi_cxt_l2_ent_vl(const struct sdxi_cxt_l2_ent *ent)
{
	return FIELD_GET(SDXI_CXT_L2_ENT_VL, le64_to_cpu(ent->lv01_ptr));
}

static dma_addr_t sdxi_cxt_l2_ent_lv01_ptr(const struct sdxi_cxt_l2_ent *ent)
{
	return FIELD_GET(SDXI_CXT_L2_ENT_LV01_PTR, le64_to_cpu(ent->lv01_ptr)) << ilog2(SZ_4K);
}

static void set_cxt_l2_entry(struct sdxi_cxt_l2_ent *l2_entry,
			     dma_addr_t l1_table_dma)
{
	u64 lv01_ptr;

	/* We shouldn't be updating a live entry. */
	if (WARN_ON_ONCE(sdxi_cxt_l2_ent_vl(l2_entry)))
		return;
	/* L1 tables must be 4K-aligned. */
	if (WARN_ON_ONCE(!IS_ALIGNED(l1_table_dma, SZ_4K)))
		return;

	lv01_ptr = (FIELD_PREP(SDXI_CXT_L2_ENT_LV01_PTR,
				   l1_table_dma >> ilog2(SZ_4K)) |
		    FIELD_PREP(SDXI_CXT_L2_ENT_VL, 1));

	/*
	 * Ensure the valid bit update follows prior updates to other
	 * control structures.
	 */
	dma_wmb();
	WRITE_ONCE(l2_entry->lv01_ptr, cpu_to_le64(lv01_ptr));
}

static void set_cxt_l1_entry(struct sdxi_dev *sdxi,
			     struct sdxi_cxt_l1_ent *l1_entry,
			     struct sdxi_cxt *cxt)
{
	u64 cxt_ctl_ptr;
	u64 akey_ptr;
	u16 intr_num;
	u32 misc0;

	if (!cxt) {
		memset(l1_entry, 0, sizeof(*l1_entry));
		return;
	}

	cxt_ctl_ptr = (FIELD_PREP(SDXI_CXT_L1_ENT_VL, 1) |
		       FIELD_PREP(SDXI_CXT_L1_ENT_KA, 1) |
		       FIELD_PREP(SDXI_CXT_L1_ENT_CXT_CTL_PTR,
				  cxt->cxt_ctl_dma >> L1_CXT_CTRL_PTR_SHIFT));
	akey_ptr = (FIELD_PREP(SDXI_CXT_L1_ENT_AKEY_SZ,
			       akey_table_order(cxt->akey_table)) |
		    FIELD_PREP(SDXI_CXT_L1_ENT_AKEY_PTR,
			       cxt->akey_table_dma >> L1_CXT_AKEY_PTR_SHIFT));
	misc0 = FIELD_PREP(SDXI_CXT_L1_ENT_MAX_BUFFER, 11);

	*l1_entry = (struct sdxi_cxt_l1_ent) {
		.cxt_ctl_ptr = cpu_to_le64(cxt_ctl_ptr),
		.akey_ptr = cpu_to_le64(akey_ptr),
		.misc0 = cpu_to_le32(misc0),
		.opb_000_enb = cpu_to_le32(sdxi->op_grp_cap),
	};

	intr_num = le16_to_cpu(cxt->akey_table->entry[0].intr_num);
	FIELD_MODIFY(SDXI_AKEY_ENT_VL, &intr_num, 1);
	cxt->akey_table->entry[0].intr_num = cpu_to_le16(intr_num);
}

static int config_cxt_tables(struct sdxi_dev *sdxi,
			     struct sdxi_cxt *cxt)
{
	struct sdxi_cxt_l1_table *l1_table;
	struct sdxi_cxt_l1_ent *l1_entry;
	u16 l2_idx;
	u8 l1_idx;

	l2_idx = ID_TO_L2_INDEX(cxt->id);
	l1_idx = ID_TO_L1_INDEX(cxt->id);

	/* Allocate L1 table if not present. */
	l1_table = sdxi->l1_table_array[l2_idx];
	if (!l1_table) {
		struct sdxi_cxt_l2_ent *l2_entry;
		dma_addr_t l1_table_dma;

		l1_table = dma_alloc_coherent(sdxi_to_dev(sdxi),
					      sizeof(*l1_table),
					      &l1_table_dma, GFP_KERNEL);
		if (!l1_table)
			return -ENOMEM;

		/* Track the L1 table vaddr. */
		sdxi->l1_table_array[l2_idx] = l1_table;

		/* Install the new entry in the L2 table. */
		l2_entry = &sdxi->l2_table->entry[l2_idx];
		set_cxt_l2_entry(l2_entry, l1_table_dma);
	}

	/* Populate the L1 entry. */
	l1_entry = &l1_table->entry[l1_idx];
	set_cxt_l1_entry(cxt->sdxi, l1_entry, cxt);

	return 0;
}

static void clear_cxt_table_entries(struct sdxi_cxt_l2_table *l2_table,
				    struct sdxi_cxt_l1_table *l1_table,
				    struct sdxi_cxt *cxt)
{
	struct sdxi_cxt_l2_ent *l2_entry;
	struct sdxi_cxt_l1_ent *l1_entry;
	struct sdxi_dev *sdxi = cxt->sdxi;
	dma_addr_t l1_dma;

	l2_entry = &l2_table->entry[ID_TO_L2_INDEX(cxt->id)];
	l1_entry = &l1_table->entry[ID_TO_L1_INDEX(cxt->id)];

	memset(l1_entry, 0, sizeof(*l1_entry));

	/* If this L1 table has been completely zeroed then free it. */
	if (memchr_inv(l1_table, 0, L1_TABLE_SIZE))
		return;

	l1_dma = sdxi_cxt_l2_ent_lv01_ptr(l2_entry);

	memset(l2_entry, 0, sizeof(*l2_entry));

	dma_free_coherent(sdxi_to_dev(sdxi), sizeof(*l1_table),
			  l1_table, l1_dma);
}

static void cleanup_cxt_tables(struct sdxi_dev *sdxi,
			       struct sdxi_cxt *cxt)
{
	u16 l2_idx;
	struct sdxi_cxt_l1_table *l1_table;

	if (!cxt)
		return;

	l2_idx = ID_TO_L2_INDEX(cxt->id);

	l1_table = sdxi->l1_table_array[l2_idx];
	/* clear l1 entry */
	/* FIXME combine clear_cxt_table_entries and this function */
	clear_cxt_table_entries(sdxi->l2_table, l1_table, cxt);
}

static struct sdxi_cxt *alloc_cxt(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt *cxt;
	u16 id, l2_idx, l1_idx;

	if (sdxi->cxt_count >= sdxi->max_cxts)
		return NULL;

	/* search for an empty context slot */
	for (id = 0; id < sdxi->max_cxts; id++) {
		l2_idx = ID_TO_L2_INDEX(id);
		l1_idx = ID_TO_L1_INDEX(id);

		if (sdxi->cxt_array[l2_idx] == NULL) {
			int sz = sizeof(struct sdxi_cxt *) * L1_TABLE_ENTRIES;
			struct sdxi_cxt **ptr = kzalloc(sz, GFP_KERNEL);

			sdxi->cxt_array[l2_idx] = ptr;
			if (!(sdxi->cxt_array[l2_idx]))
				return NULL;
		}

		cxt = (sdxi->cxt_array)[l2_idx][l1_idx];
		/* found one empty slot */
		if (!cxt)
			break;
	}

	/* nothing found, bail... */
	if (id == sdxi->max_cxts)
		return NULL;

	/* alloc context and initialize it */
	cxt = kzalloc(sizeof(struct sdxi_cxt), GFP_KERNEL);
	if (!cxt)
		return NULL;

	cxt->akey_table = dma_alloc_coherent(sdxi_to_dev(sdxi),
					     sizeof(*cxt->akey_table),
					     &cxt->akey_table_dma, GFP_KERNEL);
	if (!cxt->akey_table) {
		kfree(cxt);
		return NULL;
	}

	cxt->sdxi = sdxi;
	cxt->id = id;
	cxt->db_base = sdxi->dbs_bar + id * sdxi->db_stride;
	cxt->db = sdxi->dbs + id * sdxi->db_stride;

	sdxi->cxt_array[l2_idx][l1_idx] = cxt;
	sdxi->cxt_count++;

	return cxt;
}

static void free_cxt(struct sdxi_cxt *cxt)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	u16 l2_idx, l1_idx;

	l2_idx = ID_TO_L2_INDEX(cxt->id);
	l1_idx = ID_TO_L1_INDEX(cxt->id);

	sdxi->cxt_count--;
	dma_free_coherent(sdxi_to_dev(sdxi), sizeof(*cxt->akey_table),
			  cxt->akey_table, cxt->akey_table_dma);
	kfree(cxt);

	(sdxi->cxt_array)[l2_idx][l1_idx] = NULL;
}

/* alloc context resources and populate context table */
static struct sdxi_cxt *sdxi_cxt_alloc(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt *cxt;

	mutex_lock(&sdxi->cxt_lock);

	cxt = alloc_cxt(sdxi);
	if (!cxt)
		goto drop_cxt_lock;

	cxt->cxt_ctl = dma_pool_zalloc(sdxi->cxt_ctl_pool, GFP_KERNEL,
				       &cxt->cxt_ctl_dma);
	if (!cxt->cxt_ctl)
		goto release_cxt;

	if (config_cxt_tables(sdxi, cxt))
		goto release_cxt_ctl;

	mutex_unlock(&sdxi->cxt_lock);
	return cxt;

release_cxt_ctl:
	dma_pool_free(sdxi->cxt_ctl_pool, cxt->cxt_ctl, cxt->cxt_ctl_dma);
release_cxt:
	free_cxt(cxt);
drop_cxt_lock:
	mutex_unlock(&sdxi->cxt_lock);
	return NULL;
}

/* clear context table and free context resources */
static void sdxi_cxt_free(struct sdxi_cxt *cxt)
{
	struct sdxi_dev *sdxi = cxt->sdxi;

	mutex_lock(&sdxi->cxt_lock);

	cleanup_cxt_tables(sdxi, cxt);
	dma_pool_free(sdxi->cxt_ctl_pool, cxt->cxt_ctl, cxt->cxt_ctl_dma);
	free_cxt(cxt);

	mutex_unlock(&sdxi->cxt_lock);
}

struct sdxi_cxt *sdxi_working_cxt_init(struct sdxi_dev *sdxi,
				       enum sdxi_cxt_id id)
{
	struct sdxi_cxt *cxt;
	struct sdxi_sq *sq;

	cxt = sdxi_cxt_alloc(sdxi);
	if (!cxt) {
		sdxi_err(sdxi, "failed to alloc a new context\n");
		return NULL;
	}

	/* check if context ID matches */
	if (id < SDXI_ANY_CXT_ID && cxt->id != id) {
		sdxi_err(sdxi, "failed to alloc a context with id=%d\n", id);
		goto err_cxt_id;
	}

	sq = sdxi_sq_alloc_default(cxt);
	if (!sq) {
		sdxi_err(sdxi, "failed to alloc a submission queue (sq)\n");
		goto err_sq_alloc;
	}

	return cxt;

err_sq_alloc:
err_cxt_id:
	sdxi_cxt_free(cxt);

	return NULL;
}

static const char *cxt_sts_state_str(enum cxt_sts_state state)
{
	static const char *const context_states[] = {
		[CXTV_STOP_SW]  = "stopped (software)",
		[CXTV_RUN]      = "running",
		[CXTV_STOPG_SW] = "stopping (software)",
		[CXTV_STOP_FN]  = "stopped (function)",
		[CXTV_STOPG_FN] = "stopping (function)",
		[CXTV_ERR_FN]   = "error",
	};
	const char *str = "unknown";

	switch (state) {
	case CXTV_STOP_SW:
	case CXTV_RUN:
	case CXTV_STOPG_SW:
	case CXTV_STOP_FN:
	case CXTV_STOPG_FN:
	case CXTV_ERR_FN:
		str = context_states[state];
	}

	return str;
}

int sdxi_submit_desc(struct sdxi_cxt *cxt, const struct sdxi_desc *desc)
{
	struct sdxi_sq *sq;
	__le64 __iomem *db;
	__le64 *ring_base;
	u64 ring_entries;
	__le64 *rd_idx;
	__le64 *wr_idx;

	sq = cxt->sq;
	ring_entries = sq->ring_entries;
	ring_base = sq->desc_ring[0].qw;
	rd_idx = &sq->cxt_sts->read_index;
	wr_idx = sq->write_index;
	db = cxt->db;

	return sdxi_enqueue(desc->qw, 1, ring_base, ring_entries,
			    rd_idx, wr_idx, db);
}

static void sdxi_cxt_shutdown(struct sdxi_cxt *target_cxt)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	struct sdxi_cxt *admin_cxt = target_cxt->sdxi->admin_cxt;
	struct sdxi_dev *sdxi = target_cxt->sdxi;
	struct sdxi_cxt_sts *sts = target_cxt->sq->cxt_sts;
	struct sdxi_desc desc;
	u16 cxtid = target_cxt->id;
	struct sdxi_cxt_stop params = {
		.range = sdxi_cxt_range(cxtid),
	};
	enum cxt_sts_state state = sdxi_cxt_sts_state(sts);
	int err;

	sdxi_dbg(sdxi, "%s entry: context state: %s",
		 __func__, cxt_sts_state_str(state));

	err = sdxi_encode_cxt_stop(&desc, &params);
	if (err)
		return;

	err = sdxi_submit_desc(admin_cxt, &desc);
	if (err)
		return;

	sdxi_dbg(sdxi, "shutting down context %u\n", cxtid);

	do {
		enum cxt_sts_state state = sdxi_cxt_sts_state(sts);

		sdxi_dbg(sdxi, "context %u state: %s", cxtid,
			 cxt_sts_state_str(state));

		switch (state) {
		case CXTV_ERR_FN:
			sdxi_err(sdxi, "context %u went into error state while stopping\n",
				cxtid);
			fallthrough;
		case CXTV_STOP_SW:
		case CXTV_STOP_FN:
			return;
		case CXTV_RUN:
		case CXTV_STOPG_SW:
		case CXTV_STOPG_FN:
			/* transitional states */
			fsleep(1000);
			break;
		default:
			sdxi_err(sdxi, "context %u in unknown state %u\n",
				 cxtid, state);
			return;
		}
	} while (time_before(jiffies, deadline));

	sdxi_err(sdxi, "stopping context %u timed out (state = %u)\n",
		cxtid, sdxi_cxt_sts_state(sts));
}

void sdxi_working_cxt_exit(struct sdxi_cxt *cxt)
{
	struct sdxi_sq *sq;

	if (!cxt)
		return;

	sq = cxt->sq;
	if (!sq)
		return;

	sdxi_cxt_shutdown(cxt);

	sdxi_sq_free(sq);

	sdxi_cxt_free(cxt);
}
