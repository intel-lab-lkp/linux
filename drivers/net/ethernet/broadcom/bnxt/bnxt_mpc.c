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
	if (!bp->mpc_info)
		bp->mpc_info = kzalloc_obj(*bp->mpc_info);
	if (bp->mpc_info)
		bp->mpc_info->mpc_chnls_cap = mpc_chnls_cap;
	else
		netdev_warn(bp->dev, "Unable to allocate MPC info\n");
}

void bnxt_free_mpc_info(struct bnxt *bp)
{
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
