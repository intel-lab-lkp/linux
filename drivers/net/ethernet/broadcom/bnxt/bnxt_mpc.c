// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/stddef.h>
#include <linux/types.h>
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
