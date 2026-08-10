// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bnxt/hsi.h>

#include "bnxt.h"
#include "bnxt_crypto.h"

static u32 bnxt_get_max_crypto_key_ctx(struct bnxt *bp, int key_type)
{
	u32 fw_maj = BNXT_FW_MAJ(bp);

	if (key_type == BNXT_TX_CRYPTO_KEY_TYPE)
		return (fw_maj < 233) ? BNXT_MAX_TX_CRYPTO_KEYS_PRE_233FW :
		       BNXT_MAX_TX_CRYPTO_KEYS;

	return (fw_maj < 233) ? BNXT_MAX_RX_CRYPTO_KEYS_PRE_233FW :
	       BNXT_MAX_RX_CRYPTO_KEYS;
}

/**
 * bnxt_alloc_crypto_info - Allocate and initialize crypto offload context
 * @bp: pointer to bnxt device
 * @resp: pointer to firmware capability response
 *
 * Allocates the main crypto info structure
 *
 * This function is called during device initialization when firmware
 * reports crypto offload capability. If allocation fails, crypto offload
 * will not be available but the device will still function.
 *
 * Context: Process context
 */
void bnxt_alloc_crypto_info(struct bnxt *bp,
			    struct hwrm_func_qcaps_output *resp)
{
	u16 max_keys = le16_to_cpu(resp->max_key_ctxs_alloc);
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_kctx *kctx;
	int i;

	if (BNXT_VF(bp))
		return;
	if (!crypto) {
		crypto = kzalloc_obj(*crypto);
		if (!crypto) {
			netdev_warn(bp->dev,
				    "Unable to allocate crypto info\n");
			return;
		}
		for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
			kctx = &crypto->kctx[i];
			kctx->type = i;
		}
		bp->crypto_info = crypto;
	}
	for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
		kctx = &crypto->kctx[i];
		kctx->max_ctx = bnxt_get_max_crypto_key_ctx(bp, i);
	}
	crypto->max_key_ctxs_alloc = max_keys;
	bp->fw_cap |= BNXT_FW_CAP_KTLS;
}

/**
 * bnxt_free_crypto_info - Free crypto offload resources
 * @bp: pointer to bnxt device
 *
 * Frees all resources associated with crypto offload.  Call this function
 * only when it is idle with nothing in-flight.
 *
 * Context: Process context during device shutdown/removal
 */
void bnxt_free_crypto_info(struct bnxt *bp)
{
	kfree(bp->crypto_info);
	bp->crypto_info = NULL;
	bp->fw_cap &= ~BNXT_FW_CAP_KTLS;
}
