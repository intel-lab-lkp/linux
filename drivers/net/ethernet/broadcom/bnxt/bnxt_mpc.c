// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bnxt/hsi.h>

#include "bnxt.h"
#include "bnxt_mpc.h"

void bnxt_alloc_mpc_info(struct bnxt *bp, u8 mpc_chnls_cap)
{
	if (!(bp->flags & BNXT_FLAG_CHIP_P5_PLUS))
		return;

	if (!bp->mpc_info)
		bp->mpc_info = kzalloc_obj(*bp->mpc_info);
	if (bp->mpc_info)
		bp->mpc_info->mpc_chnls_cap = mpc_chnls_cap;
	else
		netdev_warn(bp->dev, "Unable to allocate MPC info\n");
}

void bnxt_free_mpc_info(struct bnxt *bp)
{
	bnxt_free_mpc_rings(bp);
	bnxt_free_mpcs(bp);
	kfree(bp->mpc_info);
	bp->mpc_info = NULL;
}

int bnxt_mpc_tx_rings_in_use(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, mpc_tx = 0;

	if (!mpc)
		return 0;
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++)
		mpc_tx += mpc->mpc_ring_count[i];
	return mpc_tx;
}

int bnxt_mpc_cp_rings_in_use(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;

	if (!mpc)
		return 0;
	return mpc->mpc_cp_rings;
}

bool bnxt_napi_has_mpc(struct bnxt *bp, int i)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_napi *bnapi = bp->bnapi[i];
	struct bnxt_tx_ring_info *txr;

	if (!mpc)
		return false;

	txr = bnapi->tx_ring[0];
	if (txr && !(bnapi->flags & BNXT_NAPI_FLAG_XDP))
		return txr->txq_index < mpc->mpc_cp_rings;
	return false;
}

void bnxt_set_mpc_cp_ring(struct bnxt *bp, int bnapi_idx,
			  struct bnxt_cp_ring_info *cpr)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_napi *bnapi;
	bool found = false;
	int i, j;

	if (!mpc)
		return;
	bnapi = bp->bnapi[bnapi_idx];
	/* Check both TCE and RCE MPCs for the matching NAPI */
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		for (j = 0; j < num; j++) {
			struct bnxt_tx_ring_info *txr = &mpc->mpc_rings[i][j];

			/* Only 1 ring with index j will use this NAPI */
			if (txr->bnapi == bnapi) {
				txr->tx_cpr = cpr;
				txr->tx_napi_idx = i;
				bnapi->tx_mpc_ring[i] = txr;
				found = true;
				break;
			}
		}
	}
	if (!found)
		netdev_warn_once(bp->dev, "No MPC match for napi index %d\n",
				 bnapi_idx);
	cpr->cp_ring_type = BNXT_NQ_HDL_TYPE_MP;
}

void bnxt_trim_mpc_rings(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int max = bp->tx_nr_rings_per_tc;
	u8 max_cp = 0;
	int i;

	if (!mpc)
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		mpc->mpc_ring_count[i] = min_t(int, mpc->mpc_ring_count[i],
					       max);
		max_cp = max(max_cp, mpc->mpc_ring_count[i]);
	}
	mpc->mpc_cp_rings = max_cp;
}

void bnxt_calc_dflt_mpc_rings(struct bnxt *bp, int tx_nr_rings,
			      int tx_nr_rings_per_tc, int rx_nr_rings,
			      int *mpc_per_type, int *mpc_cp)
{
	struct bnxt_hw_resc *hw_resc = &bp->hw_resc;
	int mpc_tce, avail;

	*mpc_per_type = 0;
	*mpc_cp = 0;

	if (!BNXT_MPC_CRYPTO_CAPABLE(bp))
		return;

	avail = hw_resc->max_tx_rings - tx_nr_rings;
	/* don't use more than 80% */
	avail = avail * 4 / 5;

	if (avail < (BNXT_MIN_MPC_TCE + BNXT_MIN_MPC_RCE))
		return;

	mpc_tce = min_t(int, avail / 2, tx_nr_rings_per_tc);
	mpc_tce = min_t(int, mpc_tce, BNXT_DFLT_MPC_TCE);

	avail = hw_resc->max_cp_rings - tx_nr_rings - rx_nr_rings;

	if (avail < BNXT_MIN_MPC_TCE || avail < BNXT_MIN_MPC_RCE)
		return;

	mpc_tce = min(mpc_tce, avail);

	/* TCE and RCE are sized equally, so the per-type count is also the
	 * MPC CP ring count (max(mpc_tce, mpc_rce)).
	 */
	*mpc_per_type = mpc_tce;
	*mpc_cp = mpc_tce;
}

void bnxt_set_dflt_mpc_rings(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int per_type, mpc_cp, i;

	if (!BNXT_MPC_CRYPTO_CAPABLE(bp))
		return;

	bnxt_calc_dflt_mpc_rings(bp, bp->tx_nr_rings, bp->tx_nr_rings_per_tc,
				 bp->rx_nr_rings, &per_type, &mpc_cp);

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++)
		mpc->mpc_ring_count[i] = per_type;
	mpc->mpc_cp_rings = mpc_cp;
}

void bnxt_init_mpc_ring_struct(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, j;

	if (!BNXT_MPC_CRYPTO_CAPABLE(bp))
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		if (!mpc->mpc_rings[i])
			continue;
		for (j = 0; j < num; j++) {
			struct bnxt_ring_mem_info *rmem;
			struct bnxt_ring_struct *ring;
			struct bnxt_tx_ring_info *txr;

			txr = &mpc->mpc_rings[i][j];

			txr->tx_ring_struct.ring_mem.flags =
				BNXT_RMEM_RING_PTE_FLAG;
			txr->bnapi = bp->tx_ring[bp->tx_ring_map[j]].bnapi;
			txr->txq_index = j;

			ring = &txr->tx_ring_struct;
			rmem = &ring->ring_mem;
			rmem->nr_pages = bp->tx_nr_pages;
			rmem->page_size = HW_TXBD_RING_SIZE;
			rmem->pg_arr = (void **)txr->tx_desc_ring;
			rmem->dma_arr = txr->tx_desc_mapping;
			rmem->vmem_size = SW_MPC_TXBD_RING_SIZE *
					  bp->tx_nr_pages;
			rmem->vmem = (void **)&txr->tx_mpc_buf_ring;
		}
	}
}

int bnxt_alloc_mpcs(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, rc = 0;

	if (!BNXT_MPC_CRYPTO_CAPABLE(bp))
		return 0;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];
		struct bnxt_tx_ring_info *txr;

		if (!num)
			continue;
		txr = kzalloc_objs(*txr, num);
		if (!txr) {
			rc = -ENOMEM;
			goto alloc_mpcs_exit;
		}
		mpc->mpc_rings[i] = txr;
	}

	for (i = 0; i < bp->cp_nr_rings; i++) {
		struct bnxt_napi *bnapi = bp->bnapi[i];

		if (!bnxt_napi_has_mpc(bp, i))
			continue;
		bnapi->tx_mpc_ring = kzalloc_objs(*bnapi->tx_mpc_ring,
						  BNXT_MPC_TYPE_MAX);
		if (!bnapi->tx_mpc_ring) {
			rc = -ENOMEM;
			goto alloc_mpcs_exit;
		}
	}
alloc_mpcs_exit:
	if (rc)
		bnxt_free_mpcs(bp);
	return rc;
}

void bnxt_free_mpcs(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i;

	if (!mpc)
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		kfree(mpc->mpc_rings[i]);
		mpc->mpc_rings[i] = NULL;
	}
	if (!bp->bnapi)
		return;
	for (i = 0; i < bp->cp_nr_rings; i++) {
		struct bnxt_napi *bnapi = bp->bnapi[i];

		kfree(bnapi->tx_mpc_ring);
		bnapi->tx_mpc_ring = NULL;
	}
}

int bnxt_alloc_mpc_rings(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, j, rc = 0;

	if (!mpc)
		return 0;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		for (j = 0; j < num; j++) {
			struct bnxt_tx_ring_info *txr = &mpc->mpc_rings[i][j];
			struct bnxt_ring_struct *ring;

			ring = &txr->tx_ring_struct;
			rc = bnxt_alloc_ring(bp, &ring->ring_mem);
			if (rc)
				goto alloc_mpc_rings_exit;
			ring->queue_id = BNXT_MPC_QUEUE_ID;
			ring->mpc_chnl_type = i;
			/* for stats context */
			ring->grp_idx = txr->bnapi->index;
			spin_lock_init(&txr->tx_lock);
		}
	}
alloc_mpc_rings_exit:
	if (rc)
		bnxt_free_mpc_rings(bp);
	return rc;
}

void bnxt_free_mpc_rings(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, j;

	if (!mpc)
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		if (!mpc->mpc_rings[i])
			continue;
		for (j = 0; j < num; j++) {
			struct bnxt_tx_ring_info *txr = &mpc->mpc_rings[i][j];
			struct bnxt_ring_struct *ring = &txr->tx_ring_struct;

			bnxt_free_ring(bp, &ring->ring_mem);
		}
	}
}
