/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Broadcom Inc. */

#ifndef BNXT_KTLS_H
#define BNXT_KTLS_H

#include <linux/u64_stats_sync.h>
#include <linux/wait.h>

/* Control plane counters */
enum bnxt_ktls_ctrl_counters {
	BNXT_KTLS_TX_ADD = 0,
	BNXT_KTLS_TX_DEL,

	/* Error counters for debugging */
	BNXT_KTLS_ERR_NO_MEM,			/* Memory allocation failure */
	BNXT_KTLS_ERR_NO_CAP,			/* Capability lost after FW reset */
	BNXT_KTLS_ERR_KEY_CTX_ALLOC,		/* Key context alloc failure */
	BNXT_KTLS_ERR_CRYPTO_CMD,		/* Crypto command failure */
	BNXT_KTLS_ERR_DEVICE_BUSY,		/* Device not ready */
	BNXT_KTLS_ERR_INVALID_CIPHER,		/* Unsupported cipher */
	BNXT_KTLS_ERR_STATE_NOT_OPEN,		/* Device not open */
	BNXT_KTLS_ERR_RETRY_EXCEEDED,		/* Retry limit exceeded */

	BNXT_KTLS_MAX_CTRL_COUNTERS,
};

struct bnxt_tls_info {
	atomic_t		pending;

	/* Woken from __bnxt_open_nic()/__bnxt_close_nic() when
	 * BNXT_STATE_OPEN changes, so a kTLS delete can wait out a ring
	 * reconfiguration instead of polling the state bit.
	 */
	wait_queue_head_t	open_wq;

	/* Atomic counters for control path */
	atomic64_t		*counters;
};

struct bnxt_ktls_offload_ctx_tx {
	u32		tcp_seq_no;	/* tcp seq no in sync with HW */
	u32		next_tcp_seq_no;/* staged tcp seq no */
	u32		kid;
	u32		pending_bytes;	/* staged payload bytes */
};

struct bnxt_ktls_tx_driver_state {
	struct bnxt_ktls_offload_ctx_tx *ctx_tx;
};

struct ce_add_cmd {
	__le32	ver_algo_kid_opcode;
	#define CE_ADD_CMD_OPCODE_MASK			0xfUL
	#define CE_ADD_CMD_OPCODE_SFT			0
	#define CE_ADD_CMD_OPCODE_ADD			 0x1UL
	#define CE_ADD_CMD_KID_MASK			0xfffff0UL
	#define CE_ADD_CMD_KID_SFT			4
	#define CE_ADD_CMD_ALGORITHM_MASK		0xf000000UL
	#define CE_ADD_CMD_ALGORITHM_SFT		24
	#define CE_ADD_CMD_ALGORITHM_AES_GCM_128	 0x1000000UL
	#define CE_ADD_CMD_ALGORITHM_AES_GCM_256	 0x2000000UL
	#define CE_ADD_CMD_VERSION_MASK			0xf0000000UL
	#define CE_ADD_CMD_VERSION_SFT			28
	#define CE_ADD_CMD_VERSION_TLS1_2		 (0x0UL << 28)
	#define CE_ADD_CMD_VERSION_TLS1_3		 (0x1UL << 28)
	u8	ctx_kind;
	#define CE_ADD_CMD_CTX_KIND_MASK		0x1fUL
	#define CE_ADD_CMD_CTX_KIND_SFT			0
	#define CE_ADD_CMD_CTX_KIND_CK_TX		 0x11UL
	#define CE_ADD_CMD_CTX_KIND_CK_RX		 0x12UL
	u8	unused0[3];
	u8	salt[4];
	u8	unused1[4];
	__le32	pkt_tcp_seq_num;
	__le32	tls_header_tcp_seq_num;
	u8	record_seq_num[8];
	u8	session_key[32];
	u8	addl_iv[8];
};

struct crypto_prefix_cmd {
	__le32	flags;
	#define CRYPTO_PREFIX_CMD_FLAGS_UPDATE_IN_ORDER_VAR	0x1UL
	#define CRYPTO_PREFIX_CMD_FLAGS_FULL_REPLAY_RETRAN	0x2UL
	__le32	header_tcp_seq_num;
	__le32	start_tcp_seq_num;
	__le32	end_tcp_seq_num;
	u8	explicit_nonce[8];
	u8	record_seq_num[8];
};

#define CRYPTO_PREFIX_CMD_FLAGS_UPDATE_IN_ORDER_VAR_LE	\
	cpu_to_le32(CRYPTO_PREFIX_CMD_FLAGS_UPDATE_IN_ORDER_VAR)

#define CRYPTO_PREFIX_CMD_SIZE	((u32)sizeof(struct crypto_prefix_cmd))
#define CRYPTO_PREFIX_CMD_BDS	(CRYPTO_PREFIX_CMD_SIZE / sizeof(struct tx_bd))
#define CRYPTO_PRESYNC_BDS	(CRYPTO_PREFIX_CMD_BDS + 1)

#define CRYPTO_PRESYNC_BD_CMD						\
	(cpu_to_le32((CRYPTO_PREFIX_CMD_SIZE << TX_BD_LEN_SHIFT) |	\
		     TX_BD_CNT(CRYPTO_PRESYNC_BDS) | TX_BD_TYPE_PRESYNC_TX_BD))

static inline bool bnxt_ktls_busy(struct bnxt *bp)
{
	return bp->ktls_info && atomic_read(&bp->ktls_info->pending) > 0;
}

/* Wake any kTLS control op waiting for a BNXT_STATE_OPEN transition. */
static inline void bnxt_ktls_wake(struct bnxt *bp)
{
	if (bp->ktls_info)
		wake_up_all(&bp->ktls_info->open_wq);
}

#ifdef CONFIG_BNXT_TLS
int bnxt_alloc_ktls_info(struct bnxt *bp);
void bnxt_free_ktls_info(struct bnxt *bp);
int bnxt_ktls_init(struct bnxt *bp);
struct sk_buff *bnxt_ktls_xmit(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			       struct sk_buff *skb, __le32 *lflags, u32 *kid,
			       struct bnxt_ktls_offload_ctx_tx **kctx_tx_p);
void bnxt_ktls_xmit_commit(struct bnxt_tx_ring_info *txr,
			   struct bnxt_ktls_offload_ctx_tx *kctx_tx);
int bnxt_ktls_alloc_tx_ring_stats(struct bnxt *bp,
				  struct bnxt_tx_ring_info *txr);
void bnxt_ktls_free_tx_ring_stats(struct bnxt_tx_ring_info *txr);
void bnxt_get_ring_tls_stats(struct bnxt *bp, struct bnxt_tls_sw_stats *stats);
#else
static inline int bnxt_alloc_ktls_info(struct bnxt *bp)
{
	return -EOPNOTSUPP;
}

static inline void bnxt_free_ktls_info(struct bnxt *bp)
{
}

static inline int bnxt_ktls_init(struct bnxt *bp)
{
	return -EOPNOTSUPP;
}

static inline struct sk_buff *
bnxt_ktls_xmit(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
	       struct sk_buff *skb, __le32 *lflags, u32 *kid,
	       struct bnxt_ktls_offload_ctx_tx **kctx_tx_p)
{
	return skb;
}

static inline void
bnxt_ktls_xmit_commit(struct bnxt_tx_ring_info *txr,
		      struct bnxt_ktls_offload_ctx_tx *kctx_tx)
{
}

static inline int bnxt_ktls_alloc_tx_ring_stats(struct bnxt *bp,
						struct bnxt_tx_ring_info *txr)
{
	return 0;
}

static inline void bnxt_ktls_free_tx_ring_stats(struct bnxt_tx_ring_info *txr)
{
}

static inline void bnxt_get_ring_tls_stats(struct bnxt *bp,
					   struct bnxt_tls_sw_stats *stats)
{
}
#endif	/* CONFIG_BNXT_TLS */
#endif	/* BNXT_KTLS_H */
