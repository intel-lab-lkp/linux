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
	__le64 akey_ptr;
	__le32 misc0;
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

#endif /* DMA_SDXI_HW_H */
