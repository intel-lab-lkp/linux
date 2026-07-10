// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/bnxt/hsi.h>

#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "bnxt_mpc.h"
#include "bnxt_ktls.h"
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
	char name[64];
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
		snprintf(name, sizeof(name), "bnxt_crypto-%s",
			 dev_name(&bp->pdev->dev));
		crypto->mpc_cache =
			kmem_cache_create(name,
					  sizeof(struct bnxt_crypto_cmd_ctx),
					  0, SLAB_HWCACHE_ALIGN, NULL);
		if (!crypto->mpc_cache)
			goto alloc_err;

		for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
			kctx = &crypto->kctx[i];
			kctx->type = i;
			INIT_LIST_HEAD(&kctx->list);
			spin_lock_init(&kctx->lock);
			atomic_set(&kctx->alloc_pending, 0);
			init_waitqueue_head(&kctx->alloc_pending_wq);
		}
		bp->crypto_info = crypto;
	}
	for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
		kctx = &crypto->kctx[i];
		kctx->max_ctx = bnxt_get_max_crypto_key_ctx(bp, i);
	}
	crypto->max_key_ctxs_alloc = max_keys;
	if (!bp->ktls_info)
		bnxt_alloc_ktls_info(bp);
	if (bp->ktls_info)
		bp->fw_cap |= BNXT_FW_CAP_KTLS;
	return;

alloc_err:
	kfree(crypto);
}

int bnxt_crypto_del(struct bnxt *bp, u8 type, u8 kind, u32 kid)
{
	struct bnxt_tx_ring_info *txr;
	struct ce_delete_cmd cmd = {};
	u32 data;

	if (BNXT_NO_FW_ACCESS(bp))
		return 0;

	txr = bnxt_select_mpc_ring(bp, type);
	if (!txr)
		return -ENODEV;
	if (kind == BNXT_CTX_KIND_CK_TX)
		data = CE_DELETE_CMD_CTX_KIND_CK_TX;
	else if (kind == BNXT_CTX_KIND_CK_RX)
		data = CE_DELETE_CMD_CTX_KIND_CK_RX;
	else
		return -EINVAL;

	data |= CE_DELETE_CMD_OPCODE_DEL |
		(BNXT_KID_HW(kid) << CE_DELETE_CMD_KID_SFT);

	cmd.ctx_kind_kid_opcode = cpu_to_le32(data);
	return bnxt_xmit_crypto_cmd(bp, txr, &cmd, sizeof(cmd),
				    BNXT_MPC_TMO_MSECS);
}

static void bnxt_crypto_del_all_kids(struct bnxt *bp, struct bnxt_kid_info *kid)
{
	int i, rc;

	for (i = 0; i < kid->count; i++) {
		if (!test_bit(i, kid->ids)) {
			rc = bnxt_crypto_del(bp, kid->type, kid->kind,
					     kid->start_id + i);
			if (!rc)
				set_bit(i, kid->ids);
		}
	}
}

/**
 * bnxt_crypto_del_all - Delete all crypto connections
 * @bp: pointer to bnxt device
 *
 * Delete all crypto connections and free all KIDs for re-use during
 * shutdown.  Increment the epoch counter to invalidate any outstanding
 * key references.
 *
 * Locking: the caller must have cleared BNXT_STATE_OPEN and waited for all
 * in-flight kTLS control ops to drain.  Under these preconditions there are
 * no concurrent readers or writers of the KID lists, so the lists can be
 * walked without rcu_read_lock() or the kctx lock.
 *
 * Context: Process context during shutdown/reset
 */
void bnxt_crypto_del_all(struct bnxt *bp)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_kid_info *kid;
	struct bnxt_kctx *kctx;
	int i;

	if (!crypto)
		return;

	/* No concurrent access; see the locking note above. */
	for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
		kctx = &crypto->kctx[i];
		list_for_each_entry(kid, &kctx->list, list)
			bnxt_crypto_del_all_kids(bp, kid);
		kctx->epoch = BNXT_NEXT_EPOCH(kctx->epoch);
	}
}

/**
 * bnxt_clear_crypto - Clear all crypto key contexts
 * @bp: pointer to bnxt device
 *
 * Clears all key context allocations during shutdown or firmware reset.
 * Frees all key info structures and bitmaps, and increments the epoch
 * counter to invalidate any outstanding key references.
 *
 * Locking: the caller must have cleared BNXT_STATE_OPEN and waited for all
 * in-flight kTLS control ops to drain.  Under these preconditions there are
 * no concurrent readers or writers of the KID lists, so the lists can be
 * walked without rcu_read_lock() or the kctx lock.
 *
 * Context: Process context during shutdown/reset
 */
void bnxt_clear_crypto(struct bnxt *bp)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_kid_info *kid, *tmp;
	struct bnxt_kctx *kctx;
	int i;

	if (!crypto)
		return;

	/* No concurrent access; see the locking note above. */
	for (i = 0; i < BNXT_MAX_CRYPTO_KEY_TYPE; i++) {
		kctx = &crypto->kctx[i];
		list_for_each_entry_safe(kid, tmp, &kctx->list, list) {
			list_del(&kid->list);
			kfree(kid);
		}
		kctx->total_alloc = 0;
		kctx->epoch = BNXT_NEXT_EPOCH(kctx->epoch);
	}
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
	struct bnxt_crypto_info *crypto = bp->crypto_info;

	bnxt_free_ktls_info(bp);
	if (!crypto)
		return;
	bnxt_clear_crypto(bp);
	kmem_cache_destroy(crypto->mpc_cache);
	kfree(crypto);
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

static int bnxt_key_ctx_store(struct bnxt_kctx *kctx, __le32 *key_buf, u32 num,
			      bool contig, u8 kind, u32 *id)
{
	struct bnxt_kid_info *kid;
	u32 i;

	for (i = 0; i < num; ) {
		kid = kzalloc_obj(*kid);
		/* If we cannot store the IDs, they will be lost and only
		 * reclaimed by the FW during reset/reinit.
		 */
		if (!kid)
			return -ENOMEM;
		kid->start_id = le32_to_cpu(key_buf[i]);
		kid->type = kctx->type;
		kid->kind = kind;
		if (contig)
			kid->count = num;
		else
			kid->count = 1;
		bitmap_set(kid->ids, 0, kid->count);
		if (id && !i) {
			clear_bit(0, kid->ids);
			*id = BNXT_SET_KID(kctx, kid->start_id);
		}
		spin_lock(&kctx->lock);
		list_add_tail_rcu(&kid->list, &kctx->list);
		WRITE_ONCE(kctx->total_alloc,
			   READ_ONCE(kctx->total_alloc) + kid->count);
		spin_unlock(&kctx->lock);
		i += kid->count;
	}
	return 0;
}

/* Note that the driver does not free the key contexts.  They are freed
 * by the FW during FLR and HWRM_FUNC_RESET.
 */
static int bnxt_hwrm_key_ctx_alloc(struct bnxt *bp, struct bnxt_kctx *kctx,
				   u8 kind, u32 num, u32 *id)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct hwrm_func_key_ctx_alloc_output *resp;
	struct hwrm_func_key_ctx_alloc_input *req;
	dma_addr_t mapping;
	int pending_count;
	__le32 *key_buf;
	u32 num_alloc;
	bool contig;
	int rc;

	num = min3(num, crypto->max_key_ctxs_alloc, (u32)BNXT_KID_BATCH_SIZE);
	rc = hwrm_req_init(bp, req, HWRM_FUNC_KEY_CTX_ALLOC);
	if (rc)
		return rc;

	key_buf = hwrm_req_dma_slice(bp, req, num * 4, &mapping);
	if (!key_buf) {
		rc = -ENOMEM;
		goto key_alloc_exit;
	}
	req->dma_bufr_size_bytes = cpu_to_le32(num * 4);
	req->host_dma_addr = cpu_to_le64(mapping);
	resp = hwrm_req_hold(bp, req);

	req->key_ctx_type = kctx->type;
	req->num_key_ctxs = cpu_to_le16(num);

	pending_count = atomic_inc_return(&kctx->alloc_pending);
	rc = hwrm_req_send(bp, req);
	atomic_dec(&kctx->alloc_pending);
	if (rc)
		goto key_alloc_exit_wake;

	num_alloc = le16_to_cpu(resp->num_key_ctxs_allocated);
	if (num_alloc > num) {
		netdev_warn(bp->dev,
			    "FW allocated more type %d keys (%d) than requested (%d)\n",
			    kctx->type, num_alloc, num);
	} else if (!num_alloc) {
		netdev_warn(bp->dev,
			    "FW allocated 0 type %d keys\n", kctx->type);
		rc = -ENOENT;
		goto key_alloc_exit_wake;
	} else {
		num = num_alloc;
	}
	contig = resp->flags &
		 FUNC_KEY_CTX_ALLOC_RESP_FLAGS_KEY_CTXS_CONTIGUOUS;
	rc = bnxt_key_ctx_store(kctx, key_buf, num, contig, kind, id);

key_alloc_exit_wake:
	if (pending_count >= BNXT_KCTX_ALLOC_PENDING_MAX)
		wake_up_all(&kctx->alloc_pending_wq);
key_alloc_exit:
	hwrm_req_drop(bp, req);
	return rc;
}

bool bnxt_kid_valid(struct bnxt_kctx *kctx, u32 id)
{
	struct bnxt_kid_info *kid;
	bool valid = false;
	u32 epoch;

	epoch = BNXT_KID_EPOCH(id);
	if (epoch != kctx->epoch)
		return false;

	id = BNXT_KID_HW(id);
	rcu_read_lock();
	list_for_each_entry_rcu(kid, &kctx->list, list) {
		if (id >= kid->start_id && id < kid->start_id + kid->count) {
			if (!test_bit(id - kid->start_id, kid->ids)) {
				valid = true;
				break;
			}
		}
	}
	rcu_read_unlock();
	return valid;
}

static int bnxt_alloc_one_kctx(struct bnxt_kctx *kctx, u8 kind, u32 *id)
{
	struct bnxt_kid_info *kid;
	int rc = -ENOMEM;

	rcu_read_lock();
	list_for_each_entry_rcu(kid, &kctx->list, list) {
		u32 idx = 0;

		if (kid->kind != kind)
			continue;
		do {
			idx = find_next_bit(kid->ids, kid->count, idx);
			if (idx >= kid->count)
				break;
			if (test_and_clear_bit(idx, kid->ids)) {
				*id = BNXT_SET_KID(kctx, kid->start_id + idx);
				rc = 0;
				goto alloc_done;
			}
		} while (1);
	}

alloc_done:
	rcu_read_unlock();
	return rc;
}

/**
 * bnxt_free_one_kctx - Free a key context for later re-use
 * @kctx: pointer to bnxt_kctx key context structure
 * @id: Key context ID
 *
 * This function is called to free a key context ID when the offload
 * using the ID has successfully terminated or aborted.  If the offload
 * cannot be terminated, the caller should not call this function to free
 * the ID.  The ID will only be recycled by the FW during reset/reinit.
 *
 * The ID is matched by its hardware id only (epoch stripped), so the caller
 * must ensure it is valid for the current epoch.
 */
void bnxt_free_one_kctx(struct bnxt_kctx *kctx, u32 id)
{
	struct bnxt_kid_info *kid;

	id = BNXT_KID_HW(id);
	rcu_read_lock();
	list_for_each_entry_rcu(kid, &kctx->list, list) {
		if (id >= kid->start_id && id < kid->start_id + kid->count) {
			set_bit(id - kid->start_id, kid->ids);
			break;
		}
	}
	rcu_read_unlock();
}

#define BNXT_KCTX_ALLOC_RETRY_MAX	3

int bnxt_key_ctx_alloc_one(struct bnxt *bp, struct bnxt_kctx *kctx, u8 kind,
			   u32 *id)
{
	int rc, retry = 0;

	while (retry++ < BNXT_KCTX_ALLOC_RETRY_MAX) {
		rc = bnxt_alloc_one_kctx(kctx, kind, id);
		if (!rc)
			return 0;

		/* When approaching the max, multiple threads may proceed
		 * and exceed the max.  Some may fail the serialized HWRM call
		 * later when the max is exceeded.
		 */
		if ((READ_ONCE(kctx->total_alloc) + BNXT_KID_BATCH_SIZE) >
		    kctx->max_ctx)
			return -ENOSPC;

		if (!BNXT_KCTX_ALLOC_OK(kctx)) {
			wait_event(kctx->alloc_pending_wq,
				   BNXT_KCTX_ALLOC_OK(kctx));
			continue;
		}
		rc = bnxt_hwrm_key_ctx_alloc(bp, kctx, kind,
					     BNXT_KID_BATCH_SIZE, id);
		if (!rc)
			return 0;
	}
	return -EAGAIN;
}

#define BNXT_XMIT_CRYPTO_RETRY_MAX	10
#define BNXT_XMIT_CRYPTO_MIN_TMO	100
#define BNXT_XMIT_CRYPTO_MAX_TMO	150

int bnxt_xmit_crypto_cmd(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			 void *cmd, unsigned int len, unsigned int tmo)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_crypto_cmd_ctx *ctx = NULL;
	unsigned long tmo_left, handle = 0;
	int rc, retry = 0;

	if (tmo) {
		u32 kid = CE_CMD_KID(cmd);

		ctx = kmem_cache_alloc(crypto->mpc_cache, GFP_KERNEL);
		if (!ctx)
			return -ENOMEM;
		init_completion(&ctx->cmp);
		handle = (unsigned long)ctx;
		ctx->kid = kid;
		ctx->client = txr->tx_ring_struct.mpc_chnl_type;
		ctx->status = 0;
		/* One reference for this caller, one for the handle stored in
		 * the TX buf ring.  The latter is dropped by
		 * bnxt_crypto_mpc_cmp() when the command is completed normally
		 * or after timeout.
		 */
		refcount_set(&ctx->refcnt, 2);
		retry = BNXT_XMIT_CRYPTO_RETRY_MAX;
		might_sleep();
	}
	do {
		spin_lock_bh(&txr->tx_lock);
		rc = bnxt_start_xmit_mpc(bp, txr, cmd, len, handle);
		spin_unlock_bh(&txr->tx_lock);
		if (rc == -EBUSY && tmo && retry)
			usleep_range(BNXT_XMIT_CRYPTO_MIN_TMO,
				     BNXT_XMIT_CRYPTO_MAX_TMO);
		else
			break;
	} while (retry--);
	if (rc || !tmo) {
		/* The completion will never arrive, drop one reference */
		if (ctx)
			refcount_dec(&ctx->refcnt);
		goto xmit_done;
	}

	tmo_left = wait_for_completion_timeout(&ctx->cmp, msecs_to_jiffies(tmo));
	if (!tmo_left) {
		netdev_warn(bp->dev, "crypto MP cmd %08x timed out\n",
			    *((u32 *)cmd));
		bnxt_mpc_timeout(bp, txr);
		rc = -ETIMEDOUT;
		goto xmit_done;
	}
	if (ctx->status == BNXT_CMD_CTX_COMPLETED &&
	    CE_CMPL_STATUS(&ctx->ce_cmp) == CE_CMPL_STATUS_OK)
		rc = 0;
	else
		rc = -EIO;
xmit_done:
	if (rc) {
		u8 status = ctx ? ctx->status : 0;

		netdev_warn(bp->dev,
			    "MPC transmit failed, ring idx %d, op 0x%x, kid 0x%x, status 0x%x\n",
			    txr->bnapi->index, CE_CMD_OP(cmd), CE_CMD_KID(cmd),
			    status);
	}
	if (ctx && refcount_dec_and_test(&ctx->refcnt))
		kmem_cache_free(crypto->mpc_cache, ctx);
	return rc;
}

int bnxt_crypto_init(struct bnxt *bp)
{
	struct bnxt_crypto_info *crypto = bp->crypto_info;
	struct bnxt_hw_resc *hw_resc = &bp->hw_resc;
	struct bnxt_hw_crypto_resc *crypto_resc;
	int rc;

	if (!crypto || !BNXT_SUPPORTS_KTLS(bp))
		return 0;

	crypto_resc = &hw_resc->crypto_resc;
	BNXT_TCK(crypto).max_ctx = crypto_resc->resv_tx_key_ctxs;
	BNXT_RCK(crypto).max_ctx = crypto_resc->resv_rx_key_ctxs;

	if (!BNXT_TCK(crypto).max_ctx || !BNXT_RCK(crypto).max_ctx)
		return -ENODEV;

	rc = bnxt_hwrm_key_ctx_alloc(bp, &BNXT_TCK(crypto), BNXT_CTX_KIND_CK_TX,
				     BNXT_KID_BATCH_SIZE, NULL);
	if (rc)
		return rc;

	rc = bnxt_hwrm_key_ctx_alloc(bp, &BNXT_RCK(crypto), BNXT_CTX_KIND_CK_RX,
				     BNXT_KID_BATCH_SIZE, NULL);
	if (rc)
		return rc;

	return 0;
}

void bnxt_crypto_mpc_cmp(struct bnxt *bp, u32 client, unsigned long handle,
			 struct bnxt_cmpl_entry cmpl[], u32 entries)
{
	struct bnxt_crypto_cmd_ctx *ctx;
	struct ce_cmpl *cmp = NULL;
	u32 len, kid;

	if (likely(cmpl))
		cmp = cmpl[0].cmpl;
	if (!handle || entries != 1) {
		if (entries != 1 && cmpl) {
			netdev_warn(bp->dev, "Invalid entries %d with handle %lx cmpl %08x in %s()\n",
				    entries, handle, *(u32 *)cmp, __func__);
		}
		if (!handle)
			return;
	}
	ctx = (void *)handle;
	ctx->status = BNXT_CMD_CTX_COMPLETED;
	if (unlikely(!cmpl)) {
		ctx->status |= BNXT_CMD_CTX_RESET;
		goto cmp_done;
	}
	kid = CE_CMPL_KID(cmp);
	if (ctx->kid != kid || ctx->client != client || entries != 1) {
		netdev_warn(bp->dev,
			    "Invalid CE cmpl 0x%08x with entries %d for client %d with status 0x%x, expected kid 0x%x and client %d\n",
			    *(u32 *)cmp, entries, client, ctx->status, ctx->kid,
			    ctx->client);
		ctx->status |= BNXT_CMD_CTX_ERROR;
	}
	len = min_t(u32, cmpl[0].len, sizeof(ctx->ce_cmp));
	memcpy(&ctx->ce_cmp, cmpl[0].cmpl, len);
cmp_done:
	complete(&ctx->cmp);
	if (refcount_dec_and_test(&ctx->refcnt))
		kmem_cache_free(bp->crypto_info->mpc_cache, ctx);
}
