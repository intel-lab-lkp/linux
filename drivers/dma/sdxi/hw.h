/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Advanced Micro Devices, Inc. */

/*
 * Control structures and constants defined in the SDXI specification,
 * with low-level accessors. The ordering of the structures here
 * follows the order of their definitions in the SDXI spec.
 *
 * Names of structures, members, and subfields (bit ranges within
 * members) are written to match the spec, generally. E.g. struct
 * sdxi_cxt_l2_ent corresponds to CXT_L2_ENT in the spec.
 *
 * Note: a member can have a subfield whose name is identical to the
 * member's name. E.g. CXT_L2_ENT's lv01_ptr.
 *
 * All reserved fields and bits (usually named "rsvd" or some
 * variation) must be set to zero by the driver unless otherwise
 * specified.
 */

#ifndef LINUX_SDXI_HW_H
#define LINUX_SDXI_HW_H

#include <asm/byteorder.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/log2.h>
#include <linux/stddef.h>
#include <linux/types.h>

/* Context Level 2 Table Entry (CXT_L2_ENT) */
struct sdxi_cxt_l2_ent {
	__le64 lv01_ptr;
#define SDXI_CXT_L2_ENT_LV01_PTR GENMASK_ULL(63, 12)
#define SDXI_CXT_L2_ENT_VL       BIT_ULL(0)
} __packed;
static_assert(sizeof(struct sdxi_cxt_l2_ent) == 8);

/*
 * The level 2 table is 4KB and has 512 level 1 pointer entries.
 */
#define SDXI_L2_TABLE_ENTRIES 512
struct sdxi_cxt_l2_table {
	struct sdxi_cxt_l2_ent entry[SDXI_L2_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_l2_table) == 4096);

/* Context level 1 table entry (CXT_L1_ENT) */
struct sdxi_cxt_l1_ent {
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
static_assert(sizeof(struct sdxi_cxt_l1_ent) == 32);

#define SDXI_L1_TABLE_ENTRIES 128
struct sdxi_cxt_l1_table {
	struct sdxi_cxt_l1_ent entry[SDXI_L1_TABLE_ENTRIES];
};
static_assert(sizeof(struct sdxi_cxt_l1_table) == 4096);

/* Context control block (CXT_CTL) */
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

/* Context Status (CXT_STS) */
struct sdxi_cxt_sts {
	__u8 state;
#define SDXI_CXT_STS_STATE GENMASK(3, 0)
	__u8 misc0;
	__u8 rsvd_0[6];
	__le64 read_index;
} __packed;
static_assert(sizeof(struct sdxi_cxt_sts) == 16);

/* Valid values for FIELD_GET(SDXI_CXT_STS_STATE, sdxi_cxt_sts.state) */
enum cxt_sts_state {
	CXTV_STOP_SW  = 0x0,
	CXTV_RUN      = 0x1,
	CXTV_STOPG_SW = 0x2,
	CXTV_STOP_FN  = 0x4,
	CXTV_STOPG_FN = 0x6,
	CXTV_ERR_FN   = 0xf,
};

static inline enum cxt_sts_state sdxi_cxt_sts_state(const struct sdxi_cxt_sts *sts)
{
	return FIELD_GET(SDXI_CXT_STS_STATE, READ_ONCE(sts->state));
}

/* Access key entry (AKEY_ENT) */
struct sdxi_akey_ent {
	__le16 intr_num;
#define SDXI_AKEY_ENT_VL BIT(0)
#define SDXI_AKEY_ENT_PV BIT(2)
	__le16 tgt_sfunc;
	__le32 pasid;
#define SDXI_AKEY_ENT_PASID GENMASK(19, 0)
	__le16 stag;
	__u8   rsvd_0[2];
	__le16 rkey;
	__u8   rsvd_1[2];
} __packed;
static_assert(sizeof(struct sdxi_akey_ent) == 16);

/* Error Log Header Entry (ERRLOG_HD_ENT) */
struct sdxi_errlog_hd_ent {
	__le32 opcode;
	__le16 misc0;
	__le16 cxt_num;
	__le64 dsc_index;
	__u8   rsvd_0[28];
	__le16 err_class;
	__u8   rsvd_1[2];
	__le32 vendor[4];
} __packed;
static_assert(sizeof(struct sdxi_errlog_hd_ent) == 64);

/* Completion status block (CST_BLK) */
struct sdxi_cst_blk {
	__le64 signal;
	__le32 flags;
#define SDXI_CST_BLK_ER_BIT BIT(31)
	__u8 rsvd_0[20];
} __packed;
static_assert(sizeof(struct sdxi_cst_blk) == 32);

/*
 * Size of the "body" of each descriptor between the common opcode and
 * csb_ptr fields.
 */
#define DSC_OPERATION_BYTES 52

#define define_sdxi_dsc(tag_, name_, op_body_)				\
	struct tag_ {							\
		__le32 opcode;						\
		op_body_						\
		__le64 csb_ptr;						\
	} name_;							\
	static_assert(sizeof(struct tag_) ==				\
		      sizeof(struct sdxi_dsc_generic));			\
	static_assert(offsetof(struct tag_, csb_ptr) ==			\
		      offsetof(struct sdxi_dsc_generic, csb_ptr))

struct sdxi_desc {
	union {
		__le64 qw[8];

		/* DSC_GENERIC - common header and footer */
		struct_group_tagged(sdxi_dsc_generic, generic,
			__le32 opcode;
#define SDXI_DSC_VL  BIT(0)
#define SDXI_DSC_SE  BIT(1)
#define SDXI_DSC_FE  BIT(2)
#define SDXI_DSC_CH  BIT(3)
#define SDXI_DSC_CSR BIT(4)
#define SDXI_DSC_RB  BIT(5)
#define SDXI_DSC_FLAGS   GENMASK(5, 0)
#define SDXI_DSC_SUBTYPE GENMASK(15, 8)
#define SDXI_DSC_TYPE    GENMASK(26, 16)
			__u8 operation[DSC_OPERATION_BYTES];
			__le64 csb_ptr;
#define SDXI_DSC_NP BIT_ULL(0)
#define SDXI_DSC_CSB_PTR GENMASK_ULL(63, 5)
		);

		/* DmaBaseGrp: DSC_DMAB_NOP */
		define_sdxi_dsc(sdxi_dsc_dmab_nop, nop,
			__u8 rsvd_0[DSC_OPERATION_BYTES];
		);

#define SDXI_DSC_OP_TYPE_DMAB 0x001
#define SDXI_DSC_OP_SUBTYPE_COPY 0x03
		/* DmaBaseGrp: DSC_DMAB_COPY */
		define_sdxi_dsc(sdxi_dsc_dmab_copy, copy,
				__le32 size;
				__u8 attr;
				__u8 rsvd_0[3];
				__le16 akey0;
				__le16 akey1;
				__le64 addr0;
				__le64 addr1;
				__u8 rsvd_1[24];
				);

#define SDXI_DSC_OP_TYPE_INTR 0x004
#define SDXI_DSC_OP_SUBTYPE_INTR 0x00
		/* IntrGrp: DSC_INTR */
		define_sdxi_dsc(sdxi_dsc_intr, intr,
			__u8 rsvd_0[8];
			__le16 akey;
			__u8 rsvd_1[42];
		);

#define SDXI_DSC_OP_TYPE_ADMIN 0x002
#define SDXI_DSC_OP_SUBTYPE_CXT_START_NM 0x03
#define SDXI_DSC_OP_SUBTYPE_CXT_START_RS 0x08
		/* AdminGrp: DSC_CXT_START */
		define_sdxi_dsc(sdxi_dsc_cxt_start, cxt_start,
				__u8 rsvd_0;
				__u8 vflags;
				__le16 vf_num;
				__le16 cxt_start;
				__le16 cxt_end;
				__u8 rsvd_1[4];
				__le64 db_value;
				__u8 rsvd_2[32];
				);

#define SDXI_DSC_OP_SUBTYPE_CXT_STOP     0x04
		/* AdminGrp: DSC_CXT_STOP */
		define_sdxi_dsc(sdxi_dsc_cxt_stop, cxt_stop,
			__u8 rsvd_0;
			__u8 vflags;
			__le16 vf_num;
			__le16 cxt_start;
			__le16 cxt_end;
			__u8 rsvd_1[44];
		);
	};
};
static_assert(sizeof(struct sdxi_desc) == 64);

#endif /* LINUX_SDXI_HW_H */
