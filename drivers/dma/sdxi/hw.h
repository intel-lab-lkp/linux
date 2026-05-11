/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright Advanced Micro Devices, Inc. */

/*
 * Control structures and constants defined in the SDXI specification,
 * with low-level accessors. The ordering of the structures here
 * follows the order of their definitions in the SDXI spec.
 *
 * Names of structures, members, and subfields (bit ranges within
 * members) are written to match the spec, generally. E.g. struct
 * sdxi_cxt_L2_ent corresponds to CXT_L2_ENT in the spec.
 *
 * Note: a member can have a subfield whose name is identical to the
 * member's name. E.g. CXT_L2_ENT's lv01_ptr.
 *
 * All reserved fields and bits (usually named "rsvd" or some
 * variation) must be set to zero by the driver unless otherwise
 * specified.
 */

#ifndef DMA_SDXI_HW_H
#define DMA_SDXI_HW_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <asm/byteorder.h>

/* SDXI 1.0 Table 3-2: Context Level 2 Table Entry (CXT_L2_ENT) */
struct sdxi_cxt_L2_ent {
	__le64 lv01_ptr;
#define SDXI_CXT_L2_ENT_VL       BIT_ULL(0)
#define SDXI_CXT_L2_ENT_LV01_PTR GENMASK_ULL(63, 12)
} __packed;
static_assert(sizeof(struct sdxi_cxt_L2_ent) == 8);

/* SDXI 1.0 3.2.1 Context Level 2 Table */
#define SDXI_L2_TABLE_ENTRIES 512
struct sdxi_cxt_L2_table {
	struct sdxi_cxt_L2_ent entry[SDXI_L2_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_L2_table) == 4096);

/* SDXI 1.0 Table 3-3: Context Level 1 Table Entry (CXT_L1_ENT) */
struct sdxi_cxt_L1_ent {
	__le64 cxt_ctl_ptr;
#define SDXI_CXT_L1_ENT_VL             BIT_ULL(0)
#define SDXI_CXT_L1_ENT_KA             BIT_ULL(1)
#define SDXI_CXT_L1_ENT_PV             BIT_ULL(2)
#define SDXI_CXT_L1_ENT_CXT_CTL_PTR    GENMASK_ULL(63, 6)
	__le64 akey_ptr;
#define SDXI_CXT_L1_ENT_AKEY_SZ        GENMASK_ULL(3, 0)
#define SDXI_CXT_L1_ENT_AKEY_PTR       GENMASK_ULL(63, 12)
	__le32 misc0;
#define SDXI_CXT_L1_ENT_PASID          GENMASK(19, 0)
#define SDXI_CXT_L1_ENT_MAX_BUFFER     GENMASK(23, 20)
	__le32 opb_000_enb;
	__u8 rsvd_0[8];
} __packed;
static_assert(sizeof(struct sdxi_cxt_L1_ent) == 32);

/* SDXI 1.0 3.2.2 Context Level 1 Table */
#define SDXI_L1_TABLE_ENTRIES 128
struct sdxi_cxt_L1_table {
	struct sdxi_cxt_L1_ent entry[SDXI_L1_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_L1_table) == 4096);

/* SDXI 1.0 Table 3-4: Context Control (CXT_CTL) */
struct sdxi_cxt_ctl {
	__le64 ds_ring_ptr;
#define SDXI_CXT_CTL_VL             BIT_ULL(0)
#define SDXI_CXT_CTL_QOS            GENMASK_ULL(3, 2)
#define SDXI_CXT_CTL_SE             BIT_ULL(4)
#define SDXI_CXT_CTL_CSA            BIT_ULL(5)
#define SDXI_CXT_CTL_DS_RING_PTR    GENMASK_ULL(63, 6)
	__le32 ds_ring_sz;
	__u8 rsvd_0[4];
	__le64 cxt_sts_ptr;
#define SDXI_CXT_CTL_CXT_STS_PTR    GENMASK_ULL(63, 4)
	__le64 write_index_ptr;
#define SDXI_CXT_CTL_WRITE_INDEX_PTR GENMASK_ULL(63, 3)
	__u8 rsvd_1[32];
} __packed;
static_assert(sizeof(struct sdxi_cxt_ctl) == 64);

/* SDXI 1.0 Table 3-5: Context Status (CXT_STS) */
struct sdxi_cxt_sts {
	__u8 state;
#define SDXI_CXT_STS_STATE GENMASK(3, 0)
	__u8 misc0;
	__u8 rsvd_0[6];
	__le64 read_index;
} __packed;
static_assert(sizeof(struct sdxi_cxt_sts) == 16);

/* SDXI 1.0 Table 3-6: CXT_STS.state Encoding */
/* Valid values for FIELD_GET(SDXI_CXT_STS_STATE, sdxi_cxt_sts.state). */
enum cxt_sts_state {
	CXTV_STOP_SW  = 0x0,
	CXTV_RUN      = 0x1,
	CXTV_STOPG_SW = 0x2,
	CXTV_STOP_FN  = 0x4,
	CXTV_STOPG_FN = 0x6,
	CXTV_ERR_FN   = 0xf,
};

/* SDXI 1.0 Table 3-7: AKey Table Entry (AKEY_ENT) */
struct sdxi_akey_ent {
	__le16 intr_num;
#define SDXI_AKEY_ENT_VL BIT(0)
#define SDXI_AKEY_ENT_IV BIT(1)
#define SDXI_AKEY_ENT_INTR_NUM GENMASK(14, 4)
	__le16 tgt_sfunc;
	__le32 pasid;
	__le16 stag;
	__u8   rsvd_0[2];
	__le16 rkey;
	__u8   rsvd_1[2];
} __packed;
static_assert(sizeof(struct sdxi_akey_ent) == 16);

/* SDXI 1.0 Table 6-4: CST_BLK (Completion Status Block) */
struct sdxi_cst_blk {
	__le64 signal;
	__le32 flags;
	__u8 rsvd_0[20];
} __packed;
static_assert(sizeof(struct sdxi_cst_blk) == 32);

struct sdxi_desc {
	union {
		/*
		 * SDXI 1.0 Table 6-3: DSC_GENERIC SDXI Descriptor
		 * Common Header and Footer Format
		 */
		struct_group_tagged(sdxi_dsc_generic, generic,
			__le32 opcode;
			__u8 operation[52];
			__le64 csb_ptr;
		);
	};
} __packed;
static_assert(sizeof(struct sdxi_desc) == 64);

#endif /* DMA_SDXI_HW_H */
