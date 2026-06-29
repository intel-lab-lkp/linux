/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Broadcom Inc. */

#ifndef BNXT_CRYPTO_H
#define BNXT_CRYPTO_H

#include <linux/refcount.h>

#define BNXT_MAX_TX_CRYPTO_KEYS		204800
#define BNXT_MAX_RX_CRYPTO_KEYS		204800

#define BNXT_MAX_TX_CRYPTO_KEYS_PRE_233FW	65535
#define BNXT_MAX_RX_CRYPTO_KEYS_PRE_233FW	65535

enum bnxt_crypto_type {
	BNXT_TX_CRYPTO_KEY_TYPE = FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_TX,
	BNXT_RX_CRYPTO_KEY_TYPE = FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_RX,
	BNXT_MAX_CRYPTO_KEY_TYPE,
};

#define BNXT_KID_BATCH_SIZE	128

struct bnxt_kid_info {
	struct list_head	list;
	u8			type;
	u8			kind;
	u32			start_id;
	u32			count;
	DECLARE_BITMAP(ids, BNXT_KID_BATCH_SIZE);
};

struct bnxt_kctx {
	struct list_head	list;
	/* to serialize update to the linked list and total_alloc */
	spinlock_t		lock;
	u8			type;
	u16			epoch;
	u32			total_alloc;
	u32			max_ctx;
	atomic_t		alloc_pending;
#define BNXT_KCTX_ALLOC_PENDING_MAX	8
	wait_queue_head_t	alloc_pending_wq;
};

#define BNXT_KID_HW_MASK	0x000fffff
#define BNXT_KID_HW(kid)	((kid) & BNXT_KID_HW_MASK)
#define BNXT_KID_EPOCH_MASK	0xfff00000
#define BNXT_KID_EPOCH_SHIFT	20
#define BNXT_KID_EPOCH(kid)	(((kid) & BNXT_KID_EPOCH_MASK) >>	\
				 BNXT_KID_EPOCH_SHIFT)

#define BNXT_NEXT_EPOCH(epoch)	\
	(((epoch) + 1) & (BNXT_KID_EPOCH_MASK >> BNXT_KID_EPOCH_SHIFT))

#define BNXT_SET_KID(kctx, kid)						\
	((kid) | ((u32)(kctx)->epoch << BNXT_KID_EPOCH_SHIFT))

#define BNXT_KCTX_ALLOC_OK(kctx)	\
	(atomic_read(&((kctx)->alloc_pending)) < BNXT_KCTX_ALLOC_PENDING_MAX)

struct bnxt_crypto_info {
	u16			max_key_ctxs_alloc;

	struct bnxt_kctx	kctx[BNXT_MAX_CRYPTO_KEY_TYPE];

	struct kmem_cache	*mpc_cache;
};

struct ce_delete_cmd {
	__le32  ctx_kind_kid_opcode;
	#define CE_DELETE_CMD_OPCODE_MASK		0xfUL
	#define CE_DELETE_CMD_OPCODE_SFT		0
	#define CE_DELETE_CMD_OPCODE_DEL		 0x2UL
	#define CE_DELETE_CMD_KID_MASK			0xfffff0UL
	#define CE_DELETE_CMD_KID_SFT			4
	#define CE_DELETE_CMD_CTX_KIND_MASK		0x1f000000UL
	#define CE_DELETE_CMD_CTX_KIND_SFT		24
	#define CE_DELETE_CMD_CTX_KIND_CK_TX		 (0x11UL << 24)
	#define CE_DELETE_CMD_CTX_KIND_CK_RX		 (0x12UL << 24)
};

#define CE_CMD_OP_MASK			0x00000fU
#define CE_CMD_KID_MASK			0xfffff0U
#define CE_CMD_KID_SFT			4

#define CE_CMD_OP(cmd_p)					\
	(le32_to_cpu(*(__le32 *)(cmd_p)) & CE_CMD_OP_MASK)

#define CE_CMD_KID(cmd_p)					\
	((le32_to_cpu(*(__le32 *)(cmd_p)) & CE_CMD_KID_MASK) >> CE_CMD_KID_SFT)

struct ce_cmpl {
	__le16	client_subtype_type;
	#define CE_CMPL_TYPE_MASK			0x3fUL
	#define CE_CMPL_TYPE_SFT			0
	#define CE_CMPL_TYPE_MID_PATH_SHORT		 0x1eUL
	#define CE_CMPL_SUBTYPE_MASK			0xf00UL
	#define CE_CMPL_SUBTYPE_SFT			8
	#define CE_CMPL_SUBTYPE_SOLICITED		 (0x0UL << 8)
	#define CE_CMPL_SUBTYPE_ERR			 (0x1UL << 8)
	#define CE_CMPL_SUBTYPE_RESYNC			 (0x2UL << 8)
	#define CE_CMPL_MP_CLIENT_MASK			0xf000UL
	#define CE_CMPL_MP_CLIENT_SFT			12
	#define CE_CMPL_MP_CLIENT_TCE			 (0x0UL << 12)
	#define CE_CMPL_MP_CLIENT_RCE			 (0x1UL << 12)
	__le16	status;
	#define CE_CMPL_STATUS_MASK			0xfUL
	#define CE_CMPL_STATUS_SFT			0
	#define CE_CMPL_STATUS_OK			 0x0UL
	#define CE_CMPL_STATUS_CTX_LD_ERR		 0x1UL
	#define CE_CMPL_STATUS_FID_CHK_ERR		 0x2UL
	#define CE_CMPL_STATUS_CTX_VER_ERR		 0x3UL
	#define CE_CMPL_STATUS_DST_ID_ERR		 0x4UL
	#define CE_CMPL_STATUS_MP_CMD_ERR		 0x5UL
	u32	opaque;
	__le32	v;
	#define CE_CMPL_V           0x1UL
	__le32	kid;
	#define CE_CMPL_KID_MASK    0xfffffUL
	#define CE_CMPL_KID_SFT     0
};

#define CE_CMPL_STATUS(ce_cmpl)						\
	(le16_to_cpu((ce_cmpl)->status) & CE_CMPL_STATUS_MASK)

#define CE_CMPL_KID(ce_cmpl)						\
	(le32_to_cpu((ce_cmpl)->kid) & CE_CMPL_KID_MASK)

struct bnxt_crypto_cmd_ctx {
	struct completion cmp;
	struct ce_cmpl ce_cmp;
	refcount_t refcnt;
	u32 kid;
	u16 client;
	u8 status;
#define BNXT_CMD_CTX_COMPLETED	0x1
#define BNXT_CMD_CTX_ERROR	0x2
#define BNXT_CMD_CTX_RESET	0x4
};

#define BNXT_TCK(crypto)	((crypto)->kctx[BNXT_TX_CRYPTO_KEY_TYPE])
#define BNXT_RCK(crypto)	((crypto)->kctx[BNXT_RX_CRYPTO_KEY_TYPE])

#define BNXT_CTX_KIND_CK_TX	0x11
#define BNXT_CTX_KIND_CK_RX	0x12

#ifdef CONFIG_BNXT_TLS
void bnxt_alloc_crypto_info(struct bnxt *bp,
			    struct hwrm_func_qcaps_output *resp);
int bnxt_crypto_del(struct bnxt *bp, u8 type, u8 kind, u32 kid);
void bnxt_crypto_del_all(struct bnxt *bp);
void bnxt_clear_crypto(struct bnxt *bp);
void bnxt_free_crypto_info(struct bnxt *bp);
void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req);
bool bnxt_kid_valid(struct bnxt_kctx *kctx, u32 id);
void bnxt_free_one_kctx(struct bnxt_kctx *kctx, u32 id);
int bnxt_key_ctx_alloc_one(struct bnxt *bp, struct bnxt_kctx *kctx, u8 kind,
			   u32 *id);
int bnxt_xmit_crypto_cmd(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			 void *cmd, unsigned int len, unsigned int tmo);
int bnxt_crypto_init(struct bnxt *bp);
void bnxt_crypto_mpc_cmp(struct bnxt *bp, u32 client, unsigned long handle,
			 struct bnxt_cmpl_entry cmpl[], u32 entries);
#else
static inline void bnxt_alloc_crypto_info(struct bnxt *bp,
					  struct hwrm_func_qcaps_output *resp)
{
}

static inline int bnxt_crypto_del(struct bnxt *bp, u8 type, u8 kind, u32 kid)
{
	return -EOPNOTSUPP;
}

static inline void bnxt_crypto_del_all(struct bnxt *bp)
{
}

static inline void bnxt_clear_crypto(struct bnxt *bp)
{
}

static inline void bnxt_free_crypto_info(struct bnxt *bp)
{
}

static inline void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
						 struct hwrm_func_cfg_input *req)
{
}

static inline bool bnxt_kid_valid(struct bnxt_kctx *kctx, u32 id)
{
	return false;
}

static inline void bnxt_free_one_kctx(struct bnxt_kctx *kctx, u32 id)
{
}

static inline int bnxt_key_ctx_alloc_one(struct bnxt *bp,
					 struct bnxt_kctx *kctx, u8 kind,
					 u32 *id)
{
	return -EOPNOTSUPP;
}

static inline int bnxt_xmit_crypto_cmd(struct bnxt *bp,
				       struct bnxt_tx_ring_info *txr,
				       void *cmd, unsigned int len,
				       unsigned int tmo)
{
	return -EOPNOTSUPP;
}

static inline int bnxt_crypto_init(struct bnxt *bp)
{
	return 0;
}

static inline void bnxt_crypto_mpc_cmp(struct bnxt *bp, u32 client,
				       unsigned long handle,
				       struct bnxt_cmpl_entry cmpl[],
				       u32 entries)
{
}
#endif	/* CONFIG_BNXT_TLS */
#endif	/* BNXT_CRYPTO_H */
