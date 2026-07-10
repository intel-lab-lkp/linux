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

/**
 * bnxt_hwrm_reserve_pf_key_ctxs - Reserve key contexts with firmware
 * @bp: pointer to bnxt device
 * @req: pointer to HWRM function config request
 *
 * Populates the firmware request with key context reservation parameters
 * for crypto offload based on current max settings and capabilities.
 *
 * Context: Process context during device configuration
 */
void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_hw_resc *hw_resc = &bp->hw_resc;
	struct bnxt_hw_crypto_resc *crypto_resc;
	u32 tx, rx;

	if (!crypto || !BNXT_SUPPORTS_KTLS(bp))
		return;

	crypto_resc = &hw_resc->crypto_resc;
	tx = min(BNXT_TCK(crypto).max_ctx, crypto_resc->max_tx_key_ctxs);
	rx = min(BNXT_RCK(crypto).max_ctx, crypto_resc->max_rx_key_ctxs);
	req->num_ktls_tx_key_ctxs = cpu_to_le32(tx);
	req->num_ktls_rx_key_ctxs = cpu_to_le32(rx);
	if (tx)
		req->enables |= cpu_to_le32(FUNC_CFG_REQ_ENABLES_KTLS_TX_KEY_CTXS);
	if (rx)
		req->enables |= cpu_to_le32(FUNC_CFG_REQ_ENABLES_KTLS_RX_KEY_CTXS);
}
