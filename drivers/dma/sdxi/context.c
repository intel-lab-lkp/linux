// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI context management
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt)     "SDXI: " fmt

#include <linux/bug.h>
#include <linux/cleanup.h>
#include <linux/device/devres.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "context.h"
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

	cxt->sq = kzalloc_obj(*cxt->sq, GFP_KERNEL);
	if (!cxt->sq)
		return NULL;

	cxt->akey_table = dma_alloc_coherent(dev, sizeof(*cxt->akey_table),
					     &cxt->akey_table_dma, GFP_KERNEL);
	if (!cxt->akey_table)
		return NULL;

	cxt->cxt_ctl = dma_pool_zalloc(sdxi->cxt_ctl_pool, GFP_KERNEL,
				       &cxt->cxt_ctl_dma);
	if (!cxt->cxt_ctl)
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

static void free_admin_cxt(void *ptr)
{
	struct sdxi_dev *sdxi = ptr;

	sdxi_free_cxt(sdxi->admin_cxt);
}

int sdxi_admin_cxt_init(struct sdxi_dev *sdxi)
{
	struct sdxi_cxt *cxt __free(sdxi_cxt) = sdxi_alloc_cxt(sdxi);
	if (!cxt)
		return -ENOMEM;

	cxt->id = SDXI_ADMIN_CXT_ID;
	cxt->db = sdxi->dbs + cxt->id * sdxi->db_stride;

	sdxi->admin_cxt = no_free_ptr(cxt);

	return devm_add_action_or_reset(sdxi->dev, free_admin_cxt, sdxi);
}
