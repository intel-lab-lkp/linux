// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Broadcom Inc. */

#include <linux/tcp.h>
#include <net/tls.h>
#include <linux/bnxt/hsi.h>

#include "bnxt.h"
#include "bnxt_mpc.h"
#include "bnxt_crypto.h"
#include "bnxt_ktls.h"

/**
 * bnxt_alloc_ktls_info - Allocate and initialize kTLS offload context
 * @bp: pointer to bnxt device
 *
 * Allocates the main kTLS crypto info structure
 *
 * This function is called during device initialization when firmware
 * reports kTLS offload capability. If allocation fails, kTLS offload
 * will not be available but the device will still function.
 *
 * Context: Process context
 *
 * Return: zero on success, negative error code otherwise:
 *	ENOMEM: out of memory
 */
int bnxt_alloc_ktls_info(struct bnxt *bp)
{
	struct bnxt_tls_info *ktls = bp->ktls_info;

	if (BNXT_VF(bp))
		return -EOPNOTSUPP;
	if (ktls)
		return 0;

	ktls = kzalloc_obj(*ktls);
	if (!ktls) {
		netdev_warn(bp->dev, "Unable to allocate kTLS info\n");
		return -ENOMEM;
	}
	ktls->counters = kzalloc_objs(*ktls->counters,
				      BNXT_KTLS_MAX_CTRL_COUNTERS);
	if (!ktls->counters)
		goto ktls_err;

	init_waitqueue_head(&ktls->open_wq);
	bp->ktls_info = ktls;
	return 0;

ktls_err:
	kfree(ktls->counters);
	kfree(ktls);
	return -ENOMEM;
}

/**
 * bnxt_free_ktls_info - Free kTLS crypto offload resources
 * @bp: pointer to bnxt device
 *
 * Frees all resources associated with kTLS crypto offload
 *
 * Context: Process context during device shutdown/removal
 */
void bnxt_free_ktls_info(struct bnxt *bp)
{
	struct bnxt_tls_info *ktls = bp->ktls_info;

	if (!ktls)
		return;
	kfree(ktls->counters);
	kfree(ktls);
	bp->ktls_info = NULL;
}

/* Copy in reverse byte order */
static void bnxt_copy_tls_mp_data(u8 *dst, u8 *src, int bytes)
{
	int i;

	for (i = 0; i < bytes; i++)
		dst[bytes - i - 1] = src[i];
}

static int bnxt_crypto_add(struct bnxt *bp, enum tls_offload_ctx_dir direction,
			   struct tls_crypto_info *crypto_info, u32 tcp_seq_no,
			   u32 kid)
{
	struct bnxt_tx_ring_info *txr;
	struct ce_add_cmd cmd = {0};
	u32 data;
	int rc;

	if (direction == TLS_OFFLOAD_CTX_DIR_TX) {
		txr = bnxt_select_mpc_ring(bp, BNXT_MPC_TCE_TYPE);
		cmd.ctx_kind = CE_ADD_CMD_CTX_KIND_CK_TX;
	} else {
		return -EOPNOTSUPP;
	}
	if (!txr)
		return -ENODEV;

	data = CE_ADD_CMD_OPCODE_ADD | (BNXT_KID_HW(kid) << CE_ADD_CMD_KID_SFT);
	switch (crypto_info->cipher_type) {
	case TLS_CIPHER_AES_GCM_128: {
		struct tls12_crypto_info_aes_gcm_128 *aes;

		aes = (void *)crypto_info;
		data |= CE_ADD_CMD_ALGORITHM_AES_GCM_128;
		if (crypto_info->version == TLS_1_3_VERSION)
			data |= CE_ADD_CMD_VERSION_TLS1_3;
		memcpy(&cmd.session_key, aes->key, sizeof(aes->key));
		memcpy(&cmd.salt, aes->salt, sizeof(aes->salt));
		memcpy(&cmd.addl_iv, aes->iv, sizeof(aes->iv));
		bnxt_copy_tls_mp_data(cmd.record_seq_num, aes->rec_seq,
				      sizeof(aes->rec_seq));
		break;
	}
	case TLS_CIPHER_AES_GCM_256: {
		struct tls12_crypto_info_aes_gcm_256 *aes;

		aes = (void *)crypto_info;
		data |= CE_ADD_CMD_ALGORITHM_AES_GCM_256;
		if (crypto_info->version == TLS_1_3_VERSION)
			data |= CE_ADD_CMD_VERSION_TLS1_3;
		memcpy(&cmd.session_key, aes->key, sizeof(aes->key));
		memcpy(&cmd.salt, aes->salt, sizeof(aes->salt));
		memcpy(&cmd.addl_iv, aes->iv, sizeof(aes->iv));
		bnxt_copy_tls_mp_data(cmd.record_seq_num, aes->rec_seq,
				      sizeof(aes->rec_seq));
		break;
	}
	default:
		return -EOPNOTSUPP;
	}
	cmd.ver_algo_kid_opcode = cpu_to_le32(data);
	cmd.pkt_tcp_seq_num = cpu_to_le32(tcp_seq_no);
	cmd.tls_header_tcp_seq_num = cmd.pkt_tcp_seq_num;
	rc = bnxt_xmit_crypto_cmd(bp, txr, &cmd, sizeof(cmd),
				  BNXT_MPC_TMO_MSECS);
	memzero_explicit(&cmd, sizeof(cmd));
	return rc;
}

static bool bnxt_ktls_cipher_supported(struct bnxt *bp,
				       struct tls_crypto_info *crypto_info)
{
	u16 type = crypto_info->cipher_type;
	u16 version = crypto_info->version;

	if ((type == TLS_CIPHER_AES_GCM_128 ||
	     type == TLS_CIPHER_AES_GCM_256) &&
	    (version == TLS_1_2_VERSION ||
	     version == TLS_1_3_VERSION))
		return true;
	return false;
}

static void bnxt_set_ktls_ctx_tx(struct tls_context *tls_ctx,
				 struct bnxt_ktls_offload_ctx_tx *kctx_tx)
{
	struct bnxt_ktls_tx_driver_state *tx =
		__tls_driver_ctx(tls_ctx, TLS_OFFLOAD_CTX_DIR_TX);

	tx->ctx_tx = kctx_tx;
}

static struct bnxt_ktls_offload_ctx_tx *
bnxt_get_ktls_ctx_tx(struct tls_context *tls_ctx)
{
	struct bnxt_ktls_tx_driver_state *tx =
		__tls_driver_ctx(tls_ctx, TLS_OFFLOAD_CTX_DIR_TX);

	return tx->ctx_tx;
}

static int bnxt_ktls_dev_add(struct net_device *dev, struct sock *sk,
			     enum tls_offload_ctx_dir direction,
			     struct tls_crypto_info *crypto_info,
			     u32 start_offload_tcp_sn)
{
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_crypto_info *crypto;
	struct tls_context *tls_ctx;
	struct bnxt_tls_info *ktls;
	struct bnxt_kctx *kctx;
	u32 kid;
	int rc;

	BUILD_BUG_ON(sizeof(struct bnxt_ktls_tx_driver_state) >
		     TLS_DRIVER_STATE_SIZE_TX);

	ktls = bp->ktls_info;
	if (direction == TLS_OFFLOAD_CTX_DIR_RX)
		return -EOPNOTSUPP;

	if (!BNXT_SUPPORTS_KTLS(bp)) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_NO_CAP]);
		return -EOPNOTSUPP;
	}
	atomic_inc(&ktls->pending);
	/* Make sure bnxt_close_nic() sees pending before we check the
	 * BNXT_STATE_OPEN flag.
	 */
	smp_mb__after_atomic();
	if (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_STATE_NOT_OPEN]);
		rc = -ENODEV;
		goto exit;
	}

	if (!bnxt_ktls_cipher_supported(bp, crypto_info)) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_INVALID_CIPHER]);
		rc = -EOPNOTSUPP;
		goto exit;
	}

	kctx_tx = kzalloc_obj(*kctx_tx);
	if (!kctx_tx) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_NO_MEM]);
		rc = -ENOMEM;
		goto exit;
	}
	tls_ctx = tls_get_ctx(sk);
	crypto = bp->crypto_info;
	kctx = &crypto->kctx[BNXT_TX_CRYPTO_KEY_TYPE];
	rc = bnxt_key_ctx_alloc_one(bp, kctx, BNXT_CTX_KIND_CK_TX, &kid);
	if (rc) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_KEY_CTX_ALLOC]);
		goto free_ctx;
	}
	rc = bnxt_crypto_add(bp, direction, crypto_info, start_offload_tcp_sn,
			     kid);
	if (rc) {
		atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_CRYPTO_CMD]);
		goto free_kctx;
	}
	kctx_tx->kid = kid;
	kctx_tx->tcp_seq_no = start_offload_tcp_sn;
	bnxt_set_ktls_ctx_tx(tls_ctx, kctx_tx);
	atomic64_inc(&ktls->counters[BNXT_KTLS_TX_ADD]);
	goto exit;

free_kctx:
	bnxt_free_one_kctx(kctx, kid);
free_ctx:
	kfree(kctx_tx);
exit:
	atomic_dec(&ktls->pending);
	return rc;
}

#define KTLS_RETRY_MAX		100
#define KTLS_WAIT_TMO_MS	100

static void bnxt_ktls_dev_del(struct net_device *dev,
			      struct tls_context *tls_ctx,
			      enum tls_offload_ctx_dir direction)
{
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	struct bnxt *bp = netdev_priv(dev);
	struct bnxt_crypto_info *crypto;
	struct bnxt_tls_info *ktls;
	struct bnxt_kctx *kctx;
	int retry_cnt = 0;
	u8 kind;
	u32 kid;

	ktls = bp->ktls_info;
	kctx_tx = bnxt_get_ktls_ctx_tx(tls_ctx);
retry:
	if (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		/* During ifdown or FW reset, all connections will be torn
		 * down by bnxt_crypto_del_all() / FUNC_RESET, so nothing to
		 * do here.  Only a reconfiguration is transient and
		 * __bnxt_open_nic() will set BNXT_STATE_OPEN again and wake us.
		 */
		if (!netif_running(dev) ||
		    test_bit(BNXT_STATE_IN_FW_RESET, &bp->state))
			goto free;
		/* Bound the wait so a wedged reconfig can't block the kTLS
		 * destruct work indefinitely.
		 */
		if (retry_cnt++ > KTLS_RETRY_MAX) {
			atomic64_inc(&ktls->counters[BNXT_KTLS_ERR_RETRY_EXCEEDED]);
			netdev_warn(dev, "%s timed out waiting for device, state %lx\n",
				    __func__, bp->state);
			goto free;
		}
		wait_event_timeout(ktls->open_wq,
				   test_bit(BNXT_STATE_OPEN, &bp->state) ||
				   !netif_running(dev) ||
				   test_bit(BNXT_STATE_IN_FW_RESET, &bp->state),
				   msecs_to_jiffies(KTLS_WAIT_TMO_MS));
		goto retry;
	}
	atomic_inc(&ktls->pending);
	/* Make sure bnxt_close_nic() sees pending before we check the
	 * BNXT_STATE_OPEN flag.
	 */
	smp_mb__after_atomic();
	if (!test_bit(BNXT_STATE_OPEN, &bp->state)) {
		atomic_dec(&ktls->pending);
		goto retry;
	}

	crypto = bp->crypto_info;
	kid = kctx_tx->kid;
	kctx = &crypto->kctx[BNXT_TX_CRYPTO_KEY_TYPE];
	kind = BNXT_CTX_KIND_CK_TX;
	atomic64_inc(&ktls->counters[BNXT_KTLS_TX_DEL]);
	if (bnxt_kid_valid(kctx, kid) &&
	    !bnxt_crypto_del(bp, kctx->type, kind, kid))
		bnxt_free_one_kctx(kctx, kid);

	atomic_dec(&ktls->pending);
free:
	bnxt_set_ktls_ctx_tx(tls_ctx, NULL);
	kfree(kctx_tx);
}

static const struct tlsdev_ops bnxt_ktls_ops = {
	.tls_dev_add = bnxt_ktls_dev_add,
	.tls_dev_del = bnxt_ktls_dev_del,
};

int bnxt_ktls_init(struct bnxt *bp)
{
	struct bnxt_tls_info *ktls = bp->ktls_info;
	struct net_device *dev = bp->dev;

	if (!ktls)
		return 0;

	dev->tlsdev_ops = &bnxt_ktls_ops;
	dev->hw_features |= NETIF_F_HW_TLS_TX;
	dev->features |= NETIF_F_HW_TLS_TX;
	return 0;
}

static void bnxt_ktls_inc_tx_stats(struct bnxt_tx_ring_info *txr, u32 bytes,
				   bool ooo)
{
	struct bnxt_tls_sw_stats *ring_stats = txr->tls_stats;

	if (!ring_stats)
		return;
	ring_stats->counters[BNXT_KTLS_TX_PKTS]++;
	ring_stats->counters[BNXT_KTLS_TX_BYTES] += bytes;
	if (ooo)
		ring_stats->counters[BNXT_KTLS_TX_OOO_PKTS]++;
}

static void bnxt_ktls_pre_xmit(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			       u32 kid, struct crypto_prefix_cmd *pre_cmd)
{
	struct bnxt_sw_tx_bd *tx_buf;
	struct tx_bd_presync *psbd;
	u32 bd_space, space;
	u8 *pcmd;
	u16 prod;

	prod = txr->tx_prod;
	tx_buf = &txr->tx_buf_ring[RING_TX(bp, prod)];

	psbd = (void *)&txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
	psbd->tx_bd_len_flags_type = CRYPTO_PRESYNC_BD_CMD;
	psbd->tx_bd_kid = cpu_to_le32(BNXT_KID_HW(kid));
	psbd->tx_bd_opaque =
		SET_TX_OPAQUE(bp, txr, prod, CRYPTO_PREFIX_CMD_BDS + 1);

	prod = NEXT_TX(prod);
	pcmd = (void *)&txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
	bd_space = TX_DESC_CNT - TX_IDX(prod);
	space = bd_space * sizeof(struct tx_bd);
	if (space >= CRYPTO_PREFIX_CMD_SIZE) {
		memcpy(pcmd, pre_cmd, CRYPTO_PREFIX_CMD_SIZE);
		prod += CRYPTO_PREFIX_CMD_BDS;
	} else {
		memcpy(pcmd, pre_cmd, space);
		prod += bd_space;
		pcmd = (void *)&txr->tx_desc_ring[TX_RING(bp, prod)][TX_IDX(prod)];
		memcpy(pcmd, (u8 *)pre_cmd + space,
		       CRYPTO_PREFIX_CMD_SIZE - space);
		prod += CRYPTO_PREFIX_CMD_BDS - bd_space;
	}
	txr->tx_prod = prod;
	tx_buf->is_push = 1;
	/* Minus 1 since the header psbd is a single entry short BD */
	tx_buf->inline_data_bds = CRYPTO_PREFIX_CMD_BDS - 1;
}

static int bnxt_ktls_tx_ooo(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			    struct sk_buff *skb, u32 payload_len, u32 seq,
			    struct tls_context *tls_ctx)
{
	struct bnxt_tls_sw_stats *ring_stats = txr->tls_stats;
	struct tls_offload_context_tx *tx_tls_ctx;
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	u32 hdr_tcp_seq, end_seq, total_bds;
	struct crypto_prefix_cmd pcmd = {};
	struct tls_record_info *record;
	unsigned long flags;
	bool fwd = false;
	__le64 le_rec_sn;
	u64 rec_sn;
	u8 *hdr;
	int rc;

	tx_tls_ctx = tls_offload_ctx_tx(tls_ctx);
	kctx_tx = bnxt_get_ktls_ctx_tx(tls_ctx);
	end_seq = seq + skb->len - skb_tcp_all_headers(skb);
	if (unlikely(after(seq, kctx_tx->tcp_seq_no) ||
		     after(end_seq, kctx_tx->tcp_seq_no))) {
		fwd = true;
		pcmd.flags = CRYPTO_PREFIX_CMD_FLAGS_UPDATE_IN_ORDER_VAR_LE;
	}

	spin_lock_irqsave(&tx_tls_ctx->lock, flags);
	record = tls_get_record(tx_tls_ctx, seq, &rec_sn);
	if (!record || !record->num_frags) {
		rc = -EPROTO;
		ring_stats->counters[BNXT_KTLS_TX_OOO_FALLBACK_NO_SYNC]++;
		goto unlock_exit;
	}
	hdr_tcp_seq = tls_record_start_seq(record);
	hdr = skb_frag_address_safe(&record->frags[0]);

	total_bds = CRYPTO_PRESYNC_BDS + skb_shinfo(skb)->nr_frags + 2;
	if (bnxt_tx_avail(bp, txr) < total_bds) {
		rc = -ENOSPC;
		ring_stats->counters[BNXT_KTLS_TX_OOO_FALLBACK_NO_SPACE]++;
		goto unlock_exit;
	}

	if (before(record->end_seq - tls_ctx->prot_info.tag_size,
		   seq + payload_len)) {
		/* retransmission includes tag bytes */
		rc = -EOPNOTSUPP;
		goto unlock_exit;
	}
	pcmd.header_tcp_seq_num = cpu_to_le32(hdr_tcp_seq);
	pcmd.start_tcp_seq_num = cpu_to_le32(seq);
	pcmd.end_tcp_seq_num = cpu_to_le32(seq + payload_len - 1);
	if (tls_ctx->prot_info.version == TLS_1_2_VERSION) {
		u32 nonce_bytes = tls_ctx->prot_info.iv_size;
		u32 retrans_off = seq - hdr_tcp_seq;

		if (!hdr) {
			rc = -ENOBUFS;
			ring_stats->counters[BNXT_KTLS_TX_OOO_FALLBACK_NO_HDR]++;
			goto unlock_exit;
		}
		if (retrans_off > 5 && retrans_off < 5 + nonce_bytes)
			nonce_bytes = retrans_off - 5;
		memcpy(pcmd.explicit_nonce, hdr + 5, nonce_bytes);
	}
	le_rec_sn = cpu_to_le64(rec_sn);
	memcpy(&pcmd.record_seq_num[0], &le_rec_sn, sizeof(le_rec_sn));

	rc = 0;
	bnxt_ktls_pre_xmit(bp, txr, kctx_tx->kid, &pcmd);

	if (fwd) {
		kctx_tx->next_tcp_seq_no = end_seq;
		kctx_tx->pending_fwd = 1;
	}

unlock_exit:
	spin_unlock_irqrestore(&tx_tls_ctx->lock, flags);
	return rc;
}

struct sk_buff *bnxt_ktls_xmit(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			       struct sk_buff *skb, __le32 *lflags, u32 *kid,
			       struct bnxt_ktls_offload_ctx_tx **kctx_tx_p)
{
	struct bnxt_tls_info *ktls = bp->ktls_info;
	struct bnxt_ktls_offload_ctx_tx *kctx_tx;
	struct tls_context *tls_ctx;
	u32 seq, payload_len;
	int rc;

	if (!IS_ENABLED(CONFIG_TLS_DEVICE) || !ktls ||
	    !tls_is_skb_tx_device_offloaded(skb))
		return skb;

	seq = ntohl(tcp_hdr(skb)->seq);
	tls_ctx = tls_get_ctx(skb->sk);
	kctx_tx = bnxt_get_ktls_ctx_tx(tls_ctx);
	payload_len = skb->len - skb_tcp_all_headers(skb);
	if (!payload_len)
		return skb;
	if (kctx_tx->tcp_seq_no == seq) {
		/* Stage the advance only.  tcp_seq_no and the counters are
		 * committed by bnxt_ktls_xmit_commit() once the BD reaches the
		 * ring.
		 */
		kctx_tx->next_tcp_seq_no = seq + payload_len;
		kctx_tx->pending_bytes = payload_len;
		kctx_tx->pending_ooo = 0;
		kctx_tx->pending_fwd = 1;
		*kid = BNXT_KID_HW(kctx_tx->kid);
		*kctx_tx_p = kctx_tx;
		*lflags |= cpu_to_le32(TX_BD_FLAGS_CRYPTO_EN |
				       BNXT_TX_KID_LO(*kid));
	} else {
		kctx_tx->pending_fwd = 0;
		rc = bnxt_ktls_tx_ooo(bp, txr, skb, payload_len, seq, tls_ctx);
		if (rc)
			return tls_encrypt_skb(skb);

		kctx_tx->pending_bytes = payload_len;
		kctx_tx->pending_ooo = 1;
		*kid = BNXT_KID_HW(kctx_tx->kid);
		*kctx_tx_p = kctx_tx;
		*lflags |= cpu_to_le32(TX_BD_FLAGS_CRYPTO_EN |
				       BNXT_TX_KID_LO(*kid));
		return skb;
	}
	return skb;
}

void bnxt_ktls_xmit_commit(struct bnxt_tx_ring_info *txr,
			   struct bnxt_ktls_offload_ctx_tx *kctx_tx)
{
	if (!kctx_tx)
		return;
	if (kctx_tx->pending_fwd)
		kctx_tx->tcp_seq_no = kctx_tx->next_tcp_seq_no;
	bnxt_ktls_inc_tx_stats(txr, kctx_tx->pending_bytes,
			       kctx_tx->pending_ooo);
	kctx_tx->pending_bytes = 0;
	kctx_tx->pending_fwd = 0;
	kctx_tx->pending_ooo = 0;
}

int bnxt_ktls_alloc_tx_ring_stats(struct bnxt *bp, struct bnxt_tx_ring_info *txr)
{
	struct bnxt_tls_sw_stats *ring_stats;

	if (!bp->ktls_info)
		return 0;
	ring_stats = kzalloc_obj(*ring_stats);
	if (!ring_stats)
		return -ENOMEM;
	txr->tls_stats = ring_stats;
	return 0;
}

void bnxt_ktls_free_tx_ring_stats(struct bnxt_tx_ring_info *txr)
{
	kfree(txr->tls_stats);
	txr->tls_stats = NULL;
}

void bnxt_get_ring_tls_stats(struct bnxt *bp, struct bnxt_tls_sw_stats *stats)
{
	struct bnxt_tls_sw_stats *ring_stats;
	int i, j;

	if (!bp->ktls_info || !bp->tx_ring)
		return;
	for (i = 0; i < bp->tx_nr_rings; i++) {
		ring_stats = bp->tx_ring[i].tls_stats;
		if (!ring_stats)
			continue;
		for (j = 0; j < BNXT_KTLS_MAX_DATA_COUNTERS; j++)
			stats->counters[j] += ring_stats->counters[j];
	}
}
