/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Broadcom Inc. */

#ifndef BNXT_CRYPTO_H
#define BNXT_CRYPTO_H

#define BNXT_MAX_TX_CRYPTO_KEYS		204800
#define BNXT_MAX_RX_CRYPTO_KEYS		204800

#define BNXT_MAX_TX_CRYPTO_KEYS_PRE_233FW	65535
#define BNXT_MAX_RX_CRYPTO_KEYS_PRE_233FW	65535

enum bnxt_crypto_type {
	BNXT_TX_CRYPTO_KEY_TYPE = FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_TX,
	BNXT_RX_CRYPTO_KEY_TYPE = FUNC_KEY_CTX_ALLOC_REQ_KEY_CTX_TYPE_RX,
	BNXT_MAX_CRYPTO_KEY_TYPE,
};

struct bnxt_kctx {
	u8			type;
	u32			max_ctx;
};

struct bnxt_crypto_info {
	u16			max_key_ctxs_alloc;

	struct bnxt_kctx	kctx[BNXT_MAX_CRYPTO_KEY_TYPE];
};

#define BNXT_TCK(crypto)	((crypto)->kctx[BNXT_TX_CRYPTO_KEY_TYPE])
#define BNXT_RCK(crypto)	((crypto)->kctx[BNXT_RX_CRYPTO_KEY_TYPE])

#ifdef CONFIG_BNXT_TLS
void bnxt_alloc_crypto_info(struct bnxt *bp,
			    struct hwrm_func_qcaps_output *resp);
void bnxt_free_crypto_info(struct bnxt *bp);
void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
				   struct hwrm_func_cfg_input *req);
#else
static inline void bnxt_alloc_crypto_info(struct bnxt *bp,
					  struct hwrm_func_qcaps_output *resp)
{
}

static inline void bnxt_free_crypto_info(struct bnxt *bp)
{
}

static inline void bnxt_hwrm_reserve_pf_key_ctxs(struct bnxt *bp,
						 struct hwrm_func_cfg_input *req)
{
}
#endif	/* CONFIG_BNXT_TLS */
#endif	/* BNXT_CRYPTO_H */
