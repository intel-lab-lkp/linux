// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bnxt/hsi.h>

#include "bnxt.h"
#include "bnxt_mpc.h"
#include "bnxt_crypto.h"

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

void bnxt_init_mpc_rings(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, j;

	if (!mpc)
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		for (j = 0; j < num; j++) {
			struct bnxt_tx_ring_info *txr = &mpc->mpc_rings[i][j];
			struct bnxt_ring_struct *ring = &txr->tx_ring_struct;

			txr->tx_prod = 0;
			txr->tx_cons = 0;
			txr->tx_hw_cons = 0;
			WRITE_ONCE(txr->dev_state, 0);
			ring->fw_ring_id = INVALID_HW_RING_ID;
		}
	}
}

static int bnxt_hwrm_one_mpc_ring_alloc(struct bnxt *bp,
					struct bnxt_tx_ring_info *txr)
{
	struct bnxt_cp_ring_info *cpr = txr->tx_cpr;
	struct bnxt_ring_struct *ring;
	int rc;

	ring = &cpr->cp_ring_struct;
	if (ring->fw_ring_id == INVALID_HW_RING_ID) {
		rc = bnxt_hwrm_cp_ring_alloc_p5(bp, cpr);
		if (rc)
			return rc;
	}
	/* tx_idx not used on P5_PLUS, so set it to 0 */
	return bnxt_hwrm_tx_ring_alloc(bp, txr, 0);
}

int bnxt_hwrm_mpc_ring_alloc(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int i, j, rc = 0;

	if (!mpc)
		return 0;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		int num = mpc->mpc_ring_count[i];

		for (j = 0; j < num; j++) {
			struct bnxt_tx_ring_info *txr = &mpc->mpc_rings[i][j];

			rc = bnxt_hwrm_one_mpc_ring_alloc(bp, txr);
			if (rc)
				goto mpc_ring_alloc_exit;
		}
	}
mpc_ring_alloc_exit:
	if (rc)
		bnxt_hwrm_mpc_ring_free(bp, false);
	return rc;
}

void bnxt_hwrm_mpc_ring_free(struct bnxt *bp, bool close_path)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_cp_ring_info *cpr;
	int i, j;

	if (!mpc)
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		for (j = 0; j < mpc->mpc_ring_count[i]; j++)
			bnxt_hwrm_tx_ring_free(bp, &mpc->mpc_rings[i][j],
					       close_path);
	}
	/* CP rings must be freed at the end to guarantee that the HWRM_DONE
	 * responses for HWRM_RING_FREE can still be seen on the CP rings.
	 */
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		for (j = 0; j < mpc->mpc_ring_count[i]; j++) {
			cpr = mpc->mpc_rings[i][j].tx_cpr;
			if (cpr && cpr->cp_ring_struct.fw_ring_id !=
			    INVALID_HW_RING_ID)
				bnxt_hwrm_cp_ring_free(bp, cpr);
		}
	}
}

struct bnxt_tx_ring_info *bnxt_select_mpc_ring(struct bnxt *bp, int ring_type)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	int n;

	if (!mpc || ring_type >= BNXT_MPC_TYPE_MAX ||
	    !mpc->mpc_ring_count[ring_type])
		return NULL;

	n = raw_smp_processor_id() % mpc->mpc_ring_count[ring_type];
	return &mpc->mpc_rings[ring_type][n];
}

/**
 * bnxt_start_xmit_mpc - Transmit message on an MPC ring
 * @bp: pointer to bnxt device
 * @txr: MPC TX ring structure pointer
 * @data: MPC message pointer
 * @len: MPC message length
 * @handle: Non-zero handle passed back for the completion
 *
 * This function is called to transmit an MPC message on an MPC TX ring.
 * The caller must hold txr->tx_lock.  When successful, the HW will return
 * a completion and bnxt_crypto_mpc_cmp() will be called with the handle
 * passed back.
 *
 * Return: zero on success, negative error code otherwise:
 *	ENODEV: MPC TX ring is shutting down.
 *	EBUSY: MPC TX ring is full
 */
int bnxt_start_xmit_mpc(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			void *data, unsigned int len, unsigned long handle)
{
	u32 bds, total_bds, bd_space, free_size;
	struct bnxt_sw_mpc_tx_bd *tx_buf;
	struct tx_bd *txbd;
	u16 prod;

	if (READ_ONCE(txr->dev_state) == BNXT_DEV_STATE_CLOSING)
		return -ENODEV;

	bds = DIV_ROUND_UP(len, sizeof(*txbd));
	total_bds = bds + 1;
	free_size = bnxt_tx_avail(bp, txr);
	if (free_size < total_bds)
		return -EBUSY;

	prod = txr->tx_prod;
	txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
	tx_buf = &txr->tx_mpc_buf_ring[RING_TX(bp, prod)];
	tx_buf->handle = handle;
	tx_buf->inline_bds = total_bds;

	txbd->tx_bd_len_flags_type =
		cpu_to_le32((len << TX_BD_LEN_SHIFT) | TX_BD_TYPE_MPC_TX_BD |
			    TX_BD_CNT(total_bds));
	txbd->tx_bd_opaque = SET_TX_OPAQUE(bp, txr, prod, total_bds);

	prod = NEXT_TX(prod);
	txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
	bd_space = TX_DESC_CNT - TX_IDX(prod);
	if (bd_space < bds) {
		unsigned int len0 = bd_space * sizeof(*txbd);

		memcpy(txbd, data, len0);
		prod += bd_space;
		txbd = &txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
		bds -= bd_space;
		len -= len0;
		data += len0;
	}
	memcpy(txbd, data, len);
	prod += bds;
	WRITE_ONCE(txr->tx_prod, prod);

	/* Sync BD data before updating doorbell */
	wmb();
	bnxt_db_write(bp, &txr->tx_db, prod);

	return 0;
}

/* Returns true if the ring is successfully marked as closing.  It
 * means that there will be no more MPC transmissions and NAPI will now
 * complete any MPC completions on the completion ring with NULL handles
 * to signal abort.
 */
static bool bnxt_disable_mpc_ring(struct bnxt_mpc_info *mpc, int mpc_ring)
{
	struct bnxt_tx_ring_info *txr;
	bool disabled = false;
	int i;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc_ring >= mpc->mpc_ring_count[i])
			continue;
		txr = &mpc->mpc_rings[i][mpc_ring];
		spin_lock_bh(&txr->tx_lock);
		if (!READ_ONCE(txr->dev_state)) {
			disabled = true;
			WRITE_ONCE(txr->dev_state, BNXT_DEV_STATE_CLOSING);
		}
		spin_unlock_bh(&txr->tx_lock);
		if (!disabled)
			break;
	}
	/* Make sure napi polls see @dev_state change */
	if (disabled)
		synchronize_net();
	return disabled;
}

static void bnxt_enable_mpc_ring(struct bnxt_mpc_info *mpc, int mpc_ring)
{
	struct bnxt_tx_ring_info *txr;
	int i;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc_ring >= mpc->mpc_ring_count[i])
			continue;
		txr = &mpc->mpc_rings[i][mpc_ring];
		WRITE_ONCE(txr->dev_state, 0);
	}
}

static void bnxt_clear_one_mpc_entries(struct bnxt *bp,
				       struct bnxt_tx_ring_info *txr)
{
	struct bnxt_sw_mpc_tx_bd *tx_buf;
	unsigned long handle;
	int i, max_idx;
	u32 client;

	max_idx = bp->tx_nr_pages * TX_DESC_CNT;

	for (i = 0; i < max_idx; i++) {
		tx_buf = &txr->tx_mpc_buf_ring[i];
		handle = tx_buf->handle;
		if (handle) {
			client = txr->tx_ring_struct.mpc_chnl_type;
			bnxt_crypto_mpc_cmp(bp, client, handle, NULL, 0);
			tx_buf->handle = 0;
		}
	}
}

static void bnxt_mpc_ring_stop(struct bnxt *bp, struct bnxt_mpc_info *mpc,
			       int mpc_ring)
{
	struct bnxt_tx_ring_info *txr;
	struct bnxt_cp_ring_info *cpr;
	int i;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc->mpc_ring_count[i] > mpc_ring) {
			txr = &mpc->mpc_rings[i][mpc_ring];
			bnxt_hwrm_tx_ring_free(bp, txr, true);
		}
	}
	/* CP ring must be freed at the end to guarantee that the HWRM_DONE
	 * responses for HWRM_RING_FREE can still be seen on the CP ring.
	 */
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc->mpc_ring_count[i] > mpc_ring) {
			txr = &mpc->mpc_rings[i][mpc_ring];
			cpr = txr->tx_cpr;
			if (cpr)
				bnxt_hwrm_cp_ring_free(bp, cpr);
		}
	}
	/* No new DMA to the CP ring at this point, but we need to wait
	 * for any in-flight NAPI poll to finish before we zero the
	 * completion entries in the CP ring.
	 */
	synchronize_net();
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc->mpc_ring_count[i] > mpc_ring) {
			txr = &mpc->mpc_rings[i][mpc_ring];
			cpr = txr->tx_cpr;
			if (cpr)
				bnxt_clear_one_cp_ring(bp, cpr);
			bnxt_clear_one_mpc_entries(bp, txr);
		}
	}
}

static int bnxt_mpc_ring_start(struct bnxt *bp, struct bnxt_mpc_info *mpc,
			       int mpc_ring)
{
	struct bnxt_tx_ring_info *txr;
	int i, rc;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc->mpc_ring_count[i] > mpc_ring) {
			txr = &mpc->mpc_rings[i][mpc_ring];
			txr->tx_prod = 0;
			txr->tx_cons = 0;
			txr->tx_hw_cons = 0;
			rc = bnxt_hwrm_one_mpc_ring_alloc(bp, txr);
			if (rc)
				return rc;
		}
	}
	return 0;
}

/* Called from bnxt_sp_task() with netdev_lock held.  If the device is not
 * open there is nothing to do: the open path will re-allocate and
 * re-initialize all MPC rings.
 */
void bnxt_mpc_ring_reset_task(struct bnxt *bp)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_tx_ring_info *txr;
	int i, j;

	if (!mpc || !test_bit(BNXT_STATE_OPEN, &bp->state))
		return;

	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		for (j = 0; j < mpc->mpc_ring_count[i]; j++) {
			txr = &mpc->mpc_rings[i][j];
			if (READ_ONCE(txr->dev_state) != BNXT_DEV_STATE_CLOSING)
				continue;

			/* Resets the whole ring (both types) */
			netdev_warn(bp->dev, "Resetting MPC ring %d\n", j);
			bnxt_mpc_ring_stop(bp, mpc, j);
			if (bnxt_mpc_ring_start(bp, mpc, j)) {
				netdev_err(bp->dev, "Error starting MPC ring %d, resetting device\n",
					   j);
				bnxt_mpc_ring_stop(bp, mpc, j);
				bnxt_reset_task(bp, true);
				/* bnxt_reset_task() will clear everything */
				return;
			}
			bnxt_enable_mpc_ring(mpc, j);
		}
	}
}

static int bnxt_mpc_ring_reset(struct bnxt *bp, int mpc_ring)
{
	struct bnxt_mpc_info *mpc = bp->mpc_info;
	struct bnxt_tx_ring_info *txr;
	int i;

	if (!mpc)
		return 0;
	if (mpc_ring >= mpc->mpc_cp_rings)
		return -EINVAL;

	if (!bnxt_disable_mpc_ring(mpc, mpc_ring))
		return 0;

	/* Completion handles will no longer be processed at this point
	 * from NAPI, so we can complete the outstanding commands and
	 * release the command contexts.
	 */
	for (i = 0; i < BNXT_MPC_TYPE_MAX; i++) {
		if (mpc->mpc_ring_count[i] > mpc_ring) {
			txr = &mpc->mpc_rings[i][mpc_ring];
			bnxt_clear_one_mpc_entries(bp, txr);
		}
	}

	/* If the device is going down, the reopen will re-allocate and
	 * re-initialize all MPC rings.  Otherwise defer the reset to
	 * bnxt_sp_task() to safely hold the netdev_lock.
	 */
	if (test_bit(BNXT_STATE_OPEN, &bp->state))
		bnxt_queue_sp_work(bp, BNXT_MPC_RESET_SP_EVENT);
	return 0;
}

int bnxt_mpc_timeout(struct bnxt *bp, struct bnxt_tx_ring_info *txr)
{
	if (txr->tx_ring_struct.queue_id == BNXT_MPC_QUEUE_ID)
		return bnxt_mpc_ring_reset(bp, txr->txq_index);
	return -EINVAL;
}

static bool bnxt_mpc_unsolicit(struct mpc_cmp *mpcmp)
{
	u32 client = MPC_CMP_CLIENT_TYPE(mpcmp);

	if (client != MPC_CMP_CLIENT_TCE && client != MPC_CMP_CLIENT_RCE)
		return false;
	return MPC_CMP_UNSOLICIT_SUBTYPE(mpcmp);
}

int bnxt_mpc_cmp(struct bnxt *bp, struct bnxt_cp_ring_info *cpr, u32 *raw_cons)
{
	struct bnxt_cmpl_entry cmpl_entry_arr[2];
	struct bnxt_napi *bnapi = cpr->bnapi;
	u16 cons = RING_CMP(*raw_cons);
	struct mpc_cmp *mpcmp, *mpcmp1;
	u32 tmp_raw_cons = *raw_cons;
	unsigned long handle = 0;
	u32 client, cmpl_num;
	u8 type;

	mpcmp = (struct mpc_cmp *)
		&cpr->cp_desc_ring[CP_RING(cons)][CP_IDX(cons)];
	type = MPC_CMP_CMP_TYPE(mpcmp);
	cmpl_entry_arr[0].cmpl = mpcmp;
	cmpl_entry_arr[0].len = sizeof(*mpcmp);
	cmpl_num = 1;
	if (type == MPC_CMP_TYPE_MID_PATH_LONG) {
		tmp_raw_cons = NEXT_RAW_CMP(tmp_raw_cons);
		cons = RING_CMP(tmp_raw_cons);
		mpcmp1 = (struct mpc_cmp *)
			 &cpr->cp_desc_ring[CP_RING(cons)][CP_IDX(cons)];

		if (!MPC_CMP_VALID(bp, mpcmp1, tmp_raw_cons))
			return -EBUSY;
		/* The valid test of the entry must be done first before
		 * reading any further.
		 */
		dma_rmb();
		if (mpcmp1 == mpcmp + 1) {
			cmpl_entry_arr[cmpl_num - 1].len += sizeof(*mpcmp1);
		} else {
			cmpl_entry_arr[cmpl_num].cmpl = mpcmp1;
			cmpl_entry_arr[cmpl_num].len = sizeof(*mpcmp1);
			cmpl_num++;
		}
	}
	client = MPC_CMP_CLIENT_TYPE(mpcmp) >> MPC_CMP_CLIENT_SFT;
	if (client >= BNXT_MPC_TYPE_MAX)
		goto cmp_done;

	if (!bnxt_mpc_unsolicit(mpcmp)) {
		struct bnxt_sw_mpc_tx_bd *mpc_buf;
		struct bnxt_tx_ring_info *txr;
		u16 tx_cons;
		u32 opaque;

		opaque = mpcmp->mpc_cmp_opaque;
		txr = bnapi->tx_mpc_ring[client];
		tx_cons = txr->tx_cons;
		if (TX_OPAQUE_RING(opaque) != txr->tx_napi_idx) {
			netdev_warn(bp->dev, "Wrong opaque %x, expected ring %x, cons idx %x\n",
				    opaque, txr->tx_napi_idx, txr->tx_cons);
			goto cmp_done;
		}
		mpc_buf = &txr->tx_mpc_buf_ring[RING_TX(bp, tx_cons)];
		if (!READ_ONCE(txr->dev_state)) {
			handle = mpc_buf->handle;
			mpc_buf->handle = 0;
		}
		tx_cons += mpc_buf->inline_bds;
		WRITE_ONCE(txr->tx_cons, tx_cons);
		txr->tx_hw_cons = RING_TX(bp, tx_cons);
	}
	bnxt_crypto_mpc_cmp(bp, client, handle, cmpl_entry_arr, cmpl_num);

cmp_done:
	*raw_cons = tmp_raw_cons;
	return 0;
}
