/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Broadcom Inc. */

#ifndef BNXT_MPC_H
#define BNXT_MPC_H

/* Mid path channel (MPC) definitions.  An MPC is special TX/completion
 * ring pair to send/receive control plane data to the TCE and RCE
 * (Transmit/Receive Crypto Engine) HW blocks.
 */

enum bnxt_mpc_type {
	BNXT_MPC_TCE_TYPE = RING_ALLOC_REQ_MPC_CHNLS_TYPE_TCE,
	BNXT_MPC_RCE_TYPE = RING_ALLOC_REQ_MPC_CHNLS_TYPE_RCE,
	BNXT_MPC_TYPE_MAX,
};

#define BNXT_MAX_MPC		8

#define BNXT_MIN_MPC_TCE	1
#define BNXT_MIN_MPC_RCE	1
#define BNXT_DFLT_MPC_TCE	BNXT_MAX_MPC
#define BNXT_DFLT_MPC_RCE	BNXT_MAX_MPC

struct bnxt_mpc_info {
	u8			mpc_chnls_cap;
	u8			mpc_cp_rings;
	u8			mpc_ring_count[BNXT_MPC_TYPE_MAX];
	struct bnxt_tx_ring_info *mpc_rings[BNXT_MPC_TYPE_MAX];
};

#define BNXT_MPC_CRYPTO_CAP    \
	(FUNC_QCAPS_RESP_MPC_CHNLS_CAP_TCE | FUNC_QCAPS_RESP_MPC_CHNLS_CAP_RCE)

#define BNXT_MPC_CRYPTO_CAPABLE(bp)					\
	((bp)->mpc_info ?						\
	 ((bp)->mpc_info->mpc_chnls_cap & BNXT_MPC_CRYPTO_CAP) ==	\
	  BNXT_MPC_CRYPTO_CAP : false)

#ifdef CONFIG_BNXT_TLS
void bnxt_alloc_mpc_info(struct bnxt *bp, u8 mpc_chnls_cap);
void bnxt_free_mpc_info(struct bnxt *bp);
int bnxt_mpc_tx_rings_in_use(struct bnxt *bp);
int bnxt_mpc_cp_rings_in_use(struct bnxt *bp);
void bnxt_trim_mpc_rings(struct bnxt *bp);
void bnxt_set_dflt_mpc_rings(struct bnxt *bp);
#else
static inline void bnxt_alloc_mpc_info(struct bnxt *bp, u8 mpc_chnls_cap)
{
}

static inline void bnxt_free_mpc_info(struct bnxt *bp)
{
}

static inline int bnxt_mpc_tx_rings_in_use(struct bnxt *bp)
{
	return 0;
}

static inline int bnxt_mpc_cp_rings_in_use(struct bnxt *bp)
{
	return 0;
}

static inline void bnxt_trim_mpc_rings(struct bnxt *bp)
{
}

static inline void bnxt_set_dflt_mpc_rings(struct bnxt *bp)
{
}
#endif	/* CONFIG_BNXT_TLS */
#endif	/* BNXT_MPC_H */
