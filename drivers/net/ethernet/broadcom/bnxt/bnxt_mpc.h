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

#define BNXT_MPC_TMO_MSECS      1000

struct bnxt_mpc_info {
	u8			mpc_chnls_cap;
	u8			mpc_cp_rings;
	u8			mpc_ring_count[BNXT_MPC_TYPE_MAX];
	struct bnxt_tx_ring_info *mpc_rings[BNXT_MPC_TYPE_MAX];
};

struct bnxt_sw_mpc_tx_bd {
	u8 inline_bds;
	unsigned long handle;
};

#define SW_MPC_TXBD_RING_SIZE (sizeof(struct bnxt_sw_mpc_tx_bd) * TX_DESC_CNT)

struct bnxt_cmpl_entry {
	void *cmpl;
	u32 len;
};

struct mpc_cmp {
	__le32 mpc_cmp_client_subtype_type;
	#define MPC_CMP_TYPE					(0x3f << 0)
	 #define MPC_CMP_TYPE_MID_PATH_SHORT			 0x1e
	 #define MPC_CMP_TYPE_MID_PATH_LONG			 0x1f
	#define MPC_CMP_SUBTYPE					0xf00
	#define MPC_CMP_SUBTYPE_SFT				 8
	 #define MPC_CMP_SUBTYPE_SOLICITED			 (0x0 << 8)
	 #define MPC_CMP_SUBTYPE_ERR				 (0x1 << 8)
	 #define MPC_CMP_SUBTYPE_RESYNC				 (0x2 << 8)
	#define MPC_CMP_CLIENT					(0xf << 12)
	 #define MPC_CMP_CLIENT_SFT				 12
	 #define MPC_CMP_CLIENT_TCE				 (0x0 << 12)
	 #define MPC_CMP_CLIENT_RCE				 (0x1 << 12)
	 #define MPC_CMP_CLIENT_TE_CFA				 (0x2 << 12)
	 #define MPC_CMP_CLIENT_RE_CFA				 (0x3 << 12)
	u32 mpc_cmp_opaque;
	__le32 mpc_cmp_v;
	#define MPC_CMP_V					(1 << 0)
	__le32 mpc_cmp_filler;
};

#define MPC_CMP_CMP_TYPE(mpcmp)						\
	(le32_to_cpu((mpcmp)->mpc_cmp_client_subtype_type) & MPC_CMP_TYPE)

#define MPC_CMP_CLIENT_TYPE(mpcmp)					\
	(le32_to_cpu((mpcmp)->mpc_cmp_client_subtype_type) & MPC_CMP_CLIENT)

#define MPC_CMP_UNSOLICIT_SUBTYPE(mpcmp)				\
	((le32_to_cpu((mpcmp)->mpc_cmp_client_subtype_type) &		\
	 MPC_CMP_SUBTYPE) != MPC_CMP_SUBTYPE_SOLICITED)

#define MPC_CMP_VALID(bp, mpcmp, raw_cons)				\
	(!!((mpcmp)->mpc_cmp_v & cpu_to_le32(MPC_CMP_V)) ==		\
	 !((raw_cons) & (bp)->cp_bit))

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
bool bnxt_napi_has_mpc(struct bnxt *bp, int i);
void bnxt_set_mpc_cp_ring(struct bnxt *bp, int bnapi_idx,
			  struct bnxt_cp_ring_info *cpr);
void bnxt_trim_mpc_rings(struct bnxt *bp);
void bnxt_calc_dflt_mpc_rings(struct bnxt *bp, int tx_nr_rings,
			      int tx_nr_rings_per_tc, int rx_nr_rings,
			      int *mpc_per_type, int *mpc_cp);
void bnxt_set_dflt_mpc_rings(struct bnxt *bp);
void bnxt_init_mpc_ring_struct(struct bnxt *bp);
int bnxt_alloc_mpcs(struct bnxt *bp);
void bnxt_free_mpcs(struct bnxt *bp);
int bnxt_alloc_mpc_rings(struct bnxt *bp);
void bnxt_free_mpc_rings(struct bnxt *bp);
void bnxt_init_mpc_rings(struct bnxt *bp);
int bnxt_hwrm_mpc_ring_alloc(struct bnxt *bp);
void bnxt_hwrm_mpc_ring_free(struct bnxt *bp, bool close_path);
struct bnxt_tx_ring_info *bnxt_select_mpc_ring(struct bnxt *bp, int ring_type);
int bnxt_start_xmit_mpc(struct bnxt *bp, struct bnxt_tx_ring_info *txr,
			void *data, unsigned int len, unsigned long handle);
void bnxt_mpc_ring_reset_task(struct bnxt *bp);
int bnxt_mpc_timeout(struct bnxt *bp, struct bnxt_tx_ring_info *txr);
int bnxt_mpc_cmp(struct bnxt *bp, struct bnxt_cp_ring_info *cpr, u32 *raw_cons);
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

static inline bool bnxt_napi_has_mpc(struct bnxt *bp, int i)
{
	return false;
}

static inline void bnxt_set_mpc_cp_ring(struct bnxt *bp, int bnapi_idx,
					struct bnxt_cp_ring_info *cpr)
{
}

static inline void bnxt_trim_mpc_rings(struct bnxt *bp)
{
}

static inline void bnxt_calc_dflt_mpc_rings(struct bnxt *bp, int tx_nr_rings,
					    int tx_nr_rings_per_tc,
					    int rx_nr_rings, int *mpc_per_type,
					    int *mpc_cp)
{
	*mpc_per_type = 0;
	*mpc_cp = 0;
}

static inline void bnxt_set_dflt_mpc_rings(struct bnxt *bp)
{
}

static inline void bnxt_init_mpc_ring_struct(struct bnxt *bp)
{
}

static inline int bnxt_alloc_mpcs(struct bnxt *bp)
{
	return 0;
}

static inline void bnxt_free_mpcs(struct bnxt *bp)
{
}

static inline int bnxt_alloc_mpc_rings(struct bnxt *bp)
{
	return 0;
}

static inline void bnxt_free_mpc_rings(struct bnxt *bp)
{
}

static inline void bnxt_init_mpc_rings(struct bnxt *bp)
{
}

static inline int bnxt_hwrm_mpc_ring_alloc(struct bnxt *bp)
{
	return 0;
}

static inline void bnxt_hwrm_mpc_ring_free(struct bnxt *bp, bool close_path)
{
}

static inline struct bnxt_tx_ring_info *bnxt_select_mpc_ring(struct bnxt *bp,
							     int ring_type)
{
	return NULL;
}

static inline int bnxt_start_xmit_mpc(struct bnxt *bp,
				      struct bnxt_tx_ring_info *txr, void *data,
				      unsigned int len, unsigned long handle)
{
	return -EOPNOTSUPP;
}

static inline void bnxt_mpc_ring_reset_task(struct bnxt *bp)
{
}

static inline int bnxt_mpc_timeout(struct bnxt *bp,
				   struct bnxt_tx_ring_info *txr)
{
	return -EOPNOTSUPP;
}

static inline int bnxt_mpc_cmp(struct bnxt *bp, struct bnxt_cp_ring_info *cpr,
			       u32 *raw_cons)
{
	return 0;
}
#endif	/* CONFIG_BNXT_TLS */
#endif	/* BNXT_MPC_H */
