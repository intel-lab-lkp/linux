// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI context management
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt)     "SDXI: " fmt

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/device/devres.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/rwonce.h>

#include "context.h"
#include "hw.h"
#include "ring.h"
#include "sdxi.h"

#define DEFAULT_DESC_RING_ENTRIES 1024

enum {
	/*
	 * The admin context always has ID 0. See SDXI 1.0 3.5
	 * Administrative Context (Context 0).
	 */
	SDXI_ADMIN_CXT_ID = 0,
};

/*
 * Free context and its resources. @cxt may be partially allocated but
 * must have ->sdxi set.
 */
static void sdxi_free_cxt(struct sdxi_cxt *cxt)
{
	struct sdxi_dev *sdxi = cxt->sdxi;
	struct sdxi_sq *sq = cxt->sq;

	/* Release the id if this is a client context. */
	if (cxt->id)
		WARN_ON(xa_erase(&sdxi->client_cxts, cxt->id) != cxt);

	if (cxt->cxt_ctl)
		dma_pool_free(sdxi->cxt_ctl_pool, cxt->cxt_ctl,
			      cxt->cxt_ctl_dma);
	if (cxt->akey_table)
		dma_free_coherent(sdxi->dev, sizeof(*cxt->akey_table),
				  cxt->akey_table, cxt->akey_table_dma);
	if (sq && sq->write_index)
		dma_pool_free(sdxi->write_index_pool, sq->write_index,
			      sq->write_index_dma);
	if (sq && sq->cxt_sts)
		dma_pool_free(sdxi->cxt_sts_pool, sq->cxt_sts, sq->cxt_sts_dma);
	if (sq && sq->desc_ring)
		dma_free_coherent(sdxi->dev, sq->ring_size,
				  sq->desc_ring, sq->ring_dma);
	kfree(cxt->sq);
	ida_destroy(&cxt->akey_ida);
	kfree(cxt->ring_state);
	kfree(cxt);
}

DEFINE_FREE(sdxi_cxt, struct sdxi_cxt *, if (_T) sdxi_free_cxt(_T))

/* Allocate a context and its control structure hierarchy in memory. */
static struct sdxi_cxt *sdxi_alloc_cxt(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi->dev;
	struct sdxi_sq *sq;
	struct sdxi_cxt *cxt __free(sdxi_cxt) = kzalloc(sizeof(*cxt), GFP_KERNEL);

	if (!cxt)
		return NULL;

	cxt->sdxi = sdxi;

	cxt->ring_state = kzalloc_obj(*cxt->ring_state, GFP_KERNEL);
	if (!cxt->ring_state)
		return NULL;

	cxt->sq = kzalloc_obj(*cxt->sq, GFP_KERNEL);
	if (!cxt->sq)
		return NULL;

	cxt->akey_table = dma_alloc_coherent(dev, sizeof(*cxt->akey_table),
					     &cxt->akey_table_dma, GFP_KERNEL);
	if (!cxt->akey_table)
		return NULL;

	cxt->cxt_ctl = dma_pool_zalloc(sdxi->cxt_ctl_pool, GFP_KERNEL,
				       &cxt->cxt_ctl_dma);
	if (!cxt->cxt_ctl_dma)
		return NULL;

	sq = cxt->sq;

	sq->ring_entries = DEFAULT_DESC_RING_ENTRIES;
	sq->ring_size = sq->ring_entries * sizeof(sq->desc_ring[0]);
	sq->desc_ring = dma_alloc_coherent(dev, sq->ring_size, &sq->ring_dma,
					   GFP_KERNEL);
	if (!sq->desc_ring)
		return NULL;

	sq->cxt_sts = dma_pool_zalloc(sdxi->cxt_sts_pool, GFP_KERNEL,
				      &sq->cxt_sts_dma);
	if (!sq->cxt_sts)
		return NULL;

	sq->write_index = dma_pool_zalloc(sdxi->write_index_pool, GFP_KERNEL,
					  &sq->write_index_dma);
	if (!sq->write_index)
		return NULL;

	return_ptr(cxt);
}

struct sdxi_cxt_ctl_cfg {
	dma_addr_t ds_ring_ptr;
	dma_addr_t cxt_sts_ptr;
	dma_addr_t write_index_ptr;
	u32 ds_ring_sz;
	u8 qos;
	u8 csa;
	bool se;
};

static int configure_cxt_ctl(struct sdxi_cxt_ctl *ctl, const struct sdxi_cxt_ctl_cfg *cfg)
{
	u64 ds_ring_ptr, cxt_sts_ptr, write_index_ptr;

	write_index_ptr = FIELD_PREP(SDXI_CXT_CTL_WRITE_INDEX_PTR,
				     cfg->write_index_ptr >> WRT_INDEX_PTR_SHIFT);
	cxt_sts_ptr = FIELD_PREP(SDXI_CXT_CTL_CXT_STS_PTR,
				 cfg->cxt_sts_ptr >> CXT_STATUS_PTR_SHIFT);

	*ctl = (typeof(*ctl)) {
		/*
		 * ds_ring_ptr contains the validity bit and is updated
		 * after a barrier is issued.
		 */
		.ds_ring_sz      = cpu_to_le32(cfg->ds_ring_sz),
		.cxt_sts_ptr     = cpu_to_le64(cxt_sts_ptr),
		.write_index_ptr = cpu_to_le64(write_index_ptr),
	};

	ds_ring_ptr = FIELD_PREP(SDXI_CXT_CTL_VL, 1) |
		      FIELD_PREP(SDXI_CXT_CTL_QOS, cfg->qos) |
		      FIELD_PREP(SDXI_CXT_CTL_SE, cfg->se) |
		      FIELD_PREP(SDXI_CXT_CTL_CSA, cfg->csa) |
		      FIELD_PREP(SDXI_CXT_CTL_DS_RING_PTR,
				 cfg->ds_ring_ptr >> DESC_RING_BASE_PTR_SHIFT);
	/* Ensure other fields are visible before hw sees vl=1. */
	dma_wmb();
	WRITE_ONCE(ctl->ds_ring_ptr, cpu_to_le64(ds_ring_ptr));

	return 0;
}

static void invalidate_cxtl_ctl(struct sdxi_cxt_ctl *ctl)
{
	u64 ds_ring_ptr = le64_to_cpu(ctl->ds_ring_ptr);

	FIELD_MODIFY(SDXI_CXT_CTL_VL, &ds_ring_ptr, 0);
	WRITE_ONCE(ctl->ds_ring_ptr, cpu_to_le64(ds_ring_ptr));
	dma_wmb();
	*ctl = (typeof(*ctl)) { 0 };
}

/*
 * Logical representation of CXT_L1_ENT subfields.
 */
struct sdxi_cxt_L1_cfg {
	dma_addr_t cxt_ctl_ptr;
	dma_addr_t akey_ptr;
	u32 cxt_pasid;
	u32 opb_000_enb;
	u16 max_buffer;
	u8 akey_sz;
	bool ka;
	bool pv;
};

static int configure_L1_entry(struct sdxi_cxt_L1_ent *ent,
			      const struct sdxi_cxt_L1_cfg *cfg)
{
	u64 cxt_ctl_ptr, akey_ptr;
	u32 misc0;

	if (WARN_ON_ONCE(!IS_ALIGNED(cfg->cxt_ctl_ptr, SZ_64)))
		return -EFAULT;
	if (WARN_ON_ONCE(!IS_ALIGNED(cfg->akey_ptr, SZ_4K)))
		return -EFAULT;

	akey_ptr = FIELD_PREP(SDXI_CXT_L1_ENT_AKEY_SZ, cfg->akey_sz) |
		   FIELD_PREP(SDXI_CXT_L1_ENT_AKEY_PTR,
			      cfg->akey_ptr >> L1_CXT_AKEY_PTR_SHIFT);

	misc0 = FIELD_PREP(SDXI_CXT_L1_ENT_PASID, cfg->cxt_pasid) |
		FIELD_PREP(SDXI_CXT_L1_ENT_MAX_BUFFER, cfg->max_buffer);

	*ent = (typeof(*ent)) {
		/*
		 * cxt_ctl_ptr contains the validity bit and is
		 * updated after a barrier is issued.
		 */
		.akey_ptr    = cpu_to_le64(akey_ptr),
		.misc0       = cpu_to_le32(misc0),
		.opb_000_enb = cpu_to_le32(cfg->opb_000_enb),
	};

	cxt_ctl_ptr = FIELD_PREP(SDXI_CXT_L1_ENT_VL, 1) |
		      FIELD_PREP(SDXI_CXT_L1_ENT_KA, cfg->ka) |
		      FIELD_PREP(SDXI_CXT_L1_ENT_PV, cfg->pv) |
		      FIELD_PREP(SDXI_CXT_L1_ENT_CXT_CTL_PTR,
				 cfg->cxt_ctl_ptr >> L1_CXT_CTRL_PTR_SHIFT);
	/* Ensure other fields are visible before hw sees vl=1. */
	dma_wmb();
	WRITE_ONCE(ent->cxt_ctl_ptr, cpu_to_le64(cxt_ctl_ptr));

	return 0;
}

static void invalidate_L1_entry(struct sdxi_cxt_L1_ent *ent)
{
	u64 cxt_ctl_ptr = le64_to_cpu(ent->cxt_ctl_ptr);

	FIELD_MODIFY(SDXI_CXT_L1_ENT_VL, &cxt_ctl_ptr, 0);
	WRITE_ONCE(ent->cxt_ctl_ptr, cpu_to_le64(cxt_ctl_ptr));
	dma_wmb();
	*ent = (typeof(*ent)) { 0 };
}

/*
 * Make the context control structure hierarchy valid from the POV of
 * the SDXI implementation. This may eventually involve allocation of
 * a L1 table page, so it needs to be fallible.
 */
static int sdxi_publish_cxt(const struct sdxi_cxt *cxt)
{
	struct sdxi_cxt_ctl_cfg ctl_cfg;
	struct sdxi_cxt_L1_cfg L1_cfg;
	struct sdxi_cxt_L1_ent *ent;
	u8 l1_idx;
	int err;

	if (WARN_ONCE(cxt->id > cxt->sdxi->max_cxtid,
		      "can't install cxt with id %u (limit %u)",
		      cxt->id, cxt->sdxi->max_cxtid))
		return -EINVAL;

	ctl_cfg = (typeof(ctl_cfg)) {
		.se              = 1,
		.csa             = 1,
		.ds_ring_ptr     = cxt->sq->ring_dma,
		.ds_ring_sz      = cxt->sq->ring_size >> 6,
		.cxt_sts_ptr     = cxt->sq->cxt_sts_dma,
		.write_index_ptr = cxt->sq->write_index_dma,
	};

	err = configure_cxt_ctl(cxt->cxt_ctl, &ctl_cfg);
	if (err)
		return err;

	l1_idx = ID_TO_L1_INDEX(cxt->id);

	ent = &cxt->sdxi->L1_table->entry[l1_idx];

	L1_cfg = (typeof(L1_cfg)) {
		.ka          = 1,
		.pv          = 0,
		.cxt_ctl_ptr = cxt->cxt_ctl_dma,
		.akey_sz     = akey_table_order(cxt->akey_table),
		.akey_ptr    = cxt->akey_table_dma,
		.cxt_pasid   = IOMMU_NO_PASID,
		.max_buffer  = 11, /* 4GB */
		.opb_000_enb = cxt->sdxi->op_grp_cap,
	};

	return configure_L1_entry(ent, &L1_cfg);
	/* todo: need to send DSC_CXT_UPD to admin */
}

/* Invalidate a context. */
static void sdxi_rescind_cxt(struct sdxi_cxt *cxt)
{
	u8 l1_idx = ID_TO_L1_INDEX(cxt->id);
	struct sdxi_cxt_L1_ent *ent = &cxt->sdxi->L1_table->entry[l1_idx];

	invalidate_L1_entry(ent);
	invalidate_cxtl_ctl(cxt->cxt_ctl);
	/* todo: need to send DSC_CXT_UPD to admin */
}

static void free_admin_cxt(void *ptr)
{
	struct sdxi_dev *sdxi = ptr;

	sdxi_free_cxt(sdxi->admin_cxt);
}

int sdxi_admin_cxt_init(struct sdxi_dev *sdxi)
{
	int err;
	struct sdxi_sq *sq;

	struct sdxi_cxt *cxt __free(sdxi_cxt) = sdxi_alloc_cxt(sdxi);
	if (!cxt)
		return -ENOMEM;

	sq = cxt->sq;
	/* SDXI 1.0 4.1.8.4.b: Set CXT_STS.state to CXTV_RUN. */
	sq->cxt_sts->state = FIELD_PREP(SDXI_CXT_STS_STATE, CXTV_RUN);
	cxt->id = SDXI_ADMIN_CXT_ID;
	cxt->db = sdxi->dbs + cxt->id * sdxi->db_stride;
	sdxi_ring_state_init(cxt->ring_state, &sq->cxt_sts->read_index,
			     sq->write_index, sq->ring_entries, sq->desc_ring);
	ida_init(&cxt->akey_ida);

	err = sdxi_publish_cxt(cxt);
	if (err)
		return err;

	sdxi->admin_cxt = no_free_ptr(cxt);

	return devm_add_action_or_reset(sdxi->dev, free_admin_cxt, sdxi);
}

/*
 * Temporary owner for context id until it can be assigned to a
 * context object; enables scope-based cleanup.
 */
struct sdxi_cxt_id {
	struct sdxi_dev *sdxi;
	u16 index;
};

static void sdxi_cxt_id_dtor(const struct sdxi_cxt_id *cxt_id)
{
	if (cxt_id->index == 0)
		return;
	WARN_ON(xa_erase(&cxt_id->sdxi->client_cxts, cxt_id->index) != NULL);
}

static struct sdxi_cxt_id sdxi_cxt_id_ctor(struct sdxi_dev *sdxi)
{
	struct xa_limit limit = XA_LIMIT(1, sdxi->max_cxtid);
	u32 index;

	return (struct sdxi_cxt_id) {
		.sdxi = sdxi,
		.index = xa_alloc(&sdxi->client_cxts, &index, NULL,
				  limit, GFP_KERNEL) ? 0 : (u16)index,
	};
}

DEFINE_CLASS(sdxi_cxt_id, struct sdxi_cxt_id, sdxi_cxt_id_dtor(&_T),
	     sdxi_cxt_id_ctor(sdxi), struct sdxi_dev *sdxi)

static bool sdxi_cxt_id_valid(const struct sdxi_cxt_id *cxt_id)
{
	return cxt_id->index > 0;
}

/*
 * Transfer ownership of the id to the context object, recording the
 * context pointer in the device's client_cxt xarray. sdxi_cxt_free()
 * is responsible for releasing the id from now on.
 */
static void sdxi_cxt_id_assign(struct sdxi_cxt *cxt, struct sdxi_cxt_id *cxt_id)
{
	/* We reserved the space in the constructor so this should not fail. */
	WARN_ON(xa_store(&cxt_id->sdxi->client_cxts,
			 cxt_id->index, cxt, GFP_KERNEL));
	cxt->id = cxt_id->index;
	cxt_id->index = 0;
}

/*
 * Allocate a context for in-kernel use. Starting the context is the
 * caller's responsibility.
 */
struct sdxi_cxt *sdxi_cxt_new(struct sdxi_dev *sdxi)
{
	struct sdxi_sq *sq;

	/*
	 * Ensure an ID is available before allocating memory for the
	 * context and its control structures.
	 */
	CLASS(sdxi_cxt_id, id)(sdxi);
	if (!sdxi_cxt_id_valid(&id))
		return NULL;

	struct sdxi_cxt *cxt __free(sdxi_cxt) = sdxi_alloc_cxt(sdxi);
	if (!cxt)
		return NULL;

	sdxi_cxt_id_assign(cxt, &id);

	cxt->db = sdxi->dbs + cxt->id * sdxi->db_stride;

	sq = cxt->sq;
	sdxi_ring_state_init(cxt->ring_state, &sq->cxt_sts->read_index,
			     sq->write_index, sq->ring_entries, sq->desc_ring);
	ida_init(&cxt->akey_ida);

	if (sdxi_publish_cxt(cxt))
		return NULL;

	return_ptr(cxt);
}

void sdxi_cxt_exit(struct sdxi_cxt *cxt)
{
	if (WARN_ON(sdxi_cxt_is_admin(cxt)))
		return;

	sdxi_rescind_cxt(cxt);
	sdxi_free_cxt(cxt);
}
