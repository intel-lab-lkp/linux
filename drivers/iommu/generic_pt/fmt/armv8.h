/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2026, NVIDIA CORPORATION & AFFILIATES
 *
 * The page table format described by ARM DDI 0487 L.b, Chapter D8
 * "The AArch64 Virtual Memory System Architecture" (VMSAv8-64).
 * With the right cfg this will also implement the VMSAv8-32 Long
 * Descriptor format.
 *
 * This was called io-pgtable-arm.c and ARM_xx_LPAE_Sx.
 *
 * NOTE! The level numbering is consistent with the Generic Page Table API, but
 * is reversed from what the ARM documents use.
 *
 *   generic_pt | ARM | 4K TG       | 16K TG     | 64K TG
 *   -----------+-----+-------------+------------+--------------
 *   0          |   3 | 4KB (64KB)  | 16KB (2MB) | 64KB (2MB)
 *   1          |   2 | 2MB (32MB)  | 32MB (1GB) | 512MB (16GB)
 *   2          |   1 | 1GB (16GB)  | 64GB       | 4TB (128TB)
 *   3          |   0 | 512GB (8TB) | 128TB      | -
 *   4          |  -1 | 256TB       | -          | -
 * Supported contiguous size in brackets
 */
#ifndef __GENERIC_PT_FMT_ARMV8_H
#define __GENERIC_PT_FMT_ARMV8_H

#include "defs_armv8.h"
#include "../pt_defs.h"

#include <asm/page.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/sizes.h>
#include <linux/string.h>

/*
 * Aid for understanding how the spec numerology relates to the code. Note the
 * expression "level > ARMLx" means any ARMLy such that level < x
 */
enum {
	ARML3 = 0,
	ARML2 = 1,
	ARML1 = 2,
	ARML0 = 3,
	ARMLn1 = 4,
};

enum {
	PT_MAX_TOP_LEVEL = PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA2) ? ARMLn1 :
								      ARML0,
	PT_ITEM_WORD_SIZE = sizeof(u64),
};

enum {
	PT_MAX_OUTPUT_ADDRESS_LG2 =
		(PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA2) |
		 PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA)) ? 52 : 48,
	/* See armv8pt_iommu_fmt_init() */
	PT_MAX_VA_ADDRESS_LG2 =
		(PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA2) |
		 PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LVA) |
		 PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA)) ? 52 : 48,

	/*
	 * D24.2.195 TTBR_ELx BADDR:
	 *
	 * Bits A[(x-1):0] of the stage 1 translation table base address are
	 * zero. Address bit x is the minimum address bit required to align the
	 * translation table to the size of the table. x is calculated based on
	 * LOG2(StartTableSize), as described in VMSAv9-128. The smallest
	 * permitted value of x is 5.
	 *
	 * R_KBLCR requires the table to be aligned to its own size.
	 *
	 * SMMU S2TTB additionally says: In addition, a 64-byte minimum
	 * alignment on starting-level translation table addresses is imposed
	 * when S2TG selects 64KB granules and the effective S2PS value
	 * indicates 52-bit output. In this case bits [5:0] are treated as zero.
	 */
	PT_TOP_PHYS_MASK = GENMASK_ULL(51, 5),
};

/* Common PTE bits */
enum {
	ARMV8PT_FMT_VALID = BIT(0),
	ARMV8PT_FMT_PAGE = BIT(1),
	ARMV8PT_FMT_TABLE = BIT(1),
	ARMV8PT_FMT_NS = BIT(5),
	ARMV8PT_FMT_SH = GENMASK(9, 8),
	ARMV8PT_FMT_AF = BIT(10),

	/* 64K FEAT_LPA: OA[51:48] in bits[15:12], Figures D8-14/15 */
	ARMV8PT_FMT_OA52_LPA = GENMASK_ULL(15, 12),
	/* 4K/16K FEAT_LPA2 (DS=1): OA[51:50] in bits[9:8], Figures D8-14/15 */
	ARMV8PT_FMT_OA52_LPA2 = GENMASK_ULL(9, 8),

	ARMV8PT_FMT_CONTIG = BIT_ULL(52),
	ARMV8PT_FMT_UXN = BIT_ULL(53),
	ARMV8PT_FMT_PXN = BIT_ULL(54),
	ARMV8PT_FMT_NSTABLE = BIT_ULL(63),
};

/* S1 PTE bits */
enum {
	ARMV8PT_FMT_ATTRINDX = GENMASK(4, 2),
	ARMV8PT_FMT_AP = GENMASK(7, 6),
	ARMV8PT_FMT_nG = BIT(11),
};

enum {
	ARMV8PT_SH_IS = 3,
	ARMV8PT_SH_OS = 2,

	ARMV8PT_RGN_NC = 0,
	ARMV8PT_RGN_WBWA = 1,

	ARMV8PT_AP_UNPRIV = 1,
	ARMV8PT_AP_RDONLY = 2,
};

enum {
	ARMV8PT_MAIR_ITEM = GENMASK_U32(7, 0),

	ARMV8PT_MAIR_ATTR_IDX_NC = 0,
	ARMV8PT_MAIR_ATTR_IDX_CACHE = 1,
	ARMV8PT_MAIR_ATTR_IDX_DEV = 2,
	ARMV8PT_MAIR_ATTR_IDX_INC_OCACHE = 3,

	ARMV8PT_MAIR_ATTR_NC = 0x44,
	ARMV8PT_MAIR_ATTR_WBRWA = 0xff,
	ARMV8PT_MAIR_ATTR_DEVICE = 0x04,
	ARMV8PT_MAIR_ATTR_INC_OWBRWA = 0xf4,
};

/* S2 PTE bits */
enum {
	ARMV8PT_FMT_S2MEMATTR = GENMASK(5, 2),
	ARMV8PT_FMT_S2AP = GENMASK(7, 6),
};

enum {
	SZLG2_4K = 12,
	SZLG2_16K = 14,
	SZLG2_64K = 16,
};

enum {
	/*
	 * For !S2FWB these code to:
	 *  1111 = Normal outer write back cacheable / Inner Write Back Cacheable
	 *         Permit S1 to override
	 *  0101 = Normal Non-cachable / Inner Non-cachable
	 *  0001 = Device / Device-nGnRE
	 * For S2FWB these code to:
	 *  0110 Force Normal Write Back
	 *  0101 Normal* is forced Normal-NC, Device unchanged
	 *  0001 Force Device-nGnRE
	 */
	ARMV8PT_MEMATTR_FWB_WB = 6,
	ARMV8PT_MEMATTR_OIWB = 0xf,
	ARMV8PT_MEMATTR_NC = 5,
	ARMV8PT_MEMATTR_DEV = 1,

	ARMV8PT_S2AP_READ = 1,
	ARMV8PT_S2AP_WRITE = 2,
};

#define common_to_armv8pt(common_ptr) \
	container_of_const(common_ptr, struct pt_armv8, common)
#define to_armv8pt(pts) common_to_armv8pt((pts)->range->common)

/* Runtime granule accessors */
static inline u8 armv8pt_tgsz_lg2(const struct pt_state *pts)
{
	return to_armv8pt(pts)->tgsz_lg2;
}

static inline u64 armv8pt_fmt_oa48(unsigned int tgsz_lg2)
{
	return GENMASK_ULL(47, tgsz_lg2);
}

/*
 * The generic implementations cannot be used if PT_GRANULE_LG2SZ is dynamic.
 */
static inline unsigned int armv8pt_table_item_lg2sz(const struct pt_state *pts)
{
	unsigned int tgsz_lg2 = armv8pt_tgsz_lg2(pts);

	return tgsz_lg2 + (tgsz_lg2 - ilog2(PT_ITEM_WORD_SIZE)) * pts->level;
}
#define pt_table_item_lg2sz armv8pt_table_item_lg2sz

static inline unsigned int armv8pt_pgsz_lg2_to_level(struct pt_common *common,
						     unsigned int pgsize_lg2)
{
	unsigned int tgsz_lg2 = common_to_armv8pt(common)->tgsz_lg2;

	return (pgsize_lg2 - tgsz_lg2) / (tgsz_lg2 - ilog2(sizeof(u64)));
}
#define pt_pgsz_lg2_to_level armv8pt_pgsz_lg2_to_level

static inline bool armv8pt_has_system_page_size(const struct pt_common *common)
{
	return common_to_armv8pt(common)->tgsz_lg2 == PAGE_SHIFT;
}
#define pt_has_system_page_size armv8pt_has_system_page_size

static inline pt_oaddr_t armv8pt_oa(const struct pt_state *pts)
{
	unsigned int tgsz_lg2 = armv8pt_tgsz_lg2(pts);
	u64 entry = pts->entry;
	pt_oaddr_t oa;

	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2)) {
		/* 4K/16K with DS=1: OA[49:granule] direct, OA[51:50] in [9:8] */
		oa = entry & GENMASK_ULL(49, tgsz_lg2);
		oa |= ((pt_oaddr_t)FIELD_GET(ARMV8PT_FMT_OA52_LPA2, entry))
		      << 50;
	} else {
		oa = entry & GENMASK_ULL(47, tgsz_lg2);
		if (PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA) &&
		    tgsz_lg2 == SZLG2_64K)
			oa |= ((pt_oaddr_t)FIELD_GET(ARMV8PT_FMT_OA52_LPA,
						     entry))
			      << 48;
	}
	return oa;
}

static inline pt_oaddr_t armv8pt_table_pa(const struct pt_state *pts)
{
	return armv8pt_oa(pts);
}
#define pt_table_pa armv8pt_table_pa

/*
 * Return a block or page entry pointing at a physical address. Returns the
 * address adjusted for the item in a contiguous case.
 */
static inline pt_oaddr_t armv8pt_item_oa(const struct pt_state *pts)
{
	return armv8pt_oa(pts);
}
#define pt_item_oa armv8pt_item_oa

static inline bool armv8pt_can_have_leaf(const struct pt_state *pts)
{
	unsigned int tgsz_lg2 = armv8pt_tgsz_lg2(pts);

	/*
	 * Tables D8-16/17 (4K), D8-26/27 (16K), D8-35/36 (64K).
	 *
	 * When FEAT_LPA2, one additional level gains block descriptors:
	 *   4K:  ARM level 0 (pt 3) gains 512GB blocks
	 *   16K: ARM level 1 (pt 2) gains 64GB blocks
	 * ARM Level -1 (pt 4) is always table-only.
	 */
	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2)) {
		if (tgsz_lg2 == SZLG2_4K && pts->level > ARML0)
			return false;
		if (tgsz_lg2 == SZLG2_16K && pts->level > ARML1)
			return false;
		if (tgsz_lg2 == SZLG2_64K && pts->level > ARML2)
			return false;
	} else {
		if (tgsz_lg2 == SZLG2_4K && pts->level > ARML1)
			return false;
		if (tgsz_lg2 == SZLG2_16K && pts->level > ARML2)
			return false;
		if (tgsz_lg2 == SZLG2_64K &&
		    pts->level > (pts_feature(pts, PT_FEAT_ARMV8_LPA) ? ARML1 :
									ARML2))
			return false;
	}
	return true;
}
#define pt_can_have_leaf armv8pt_can_have_leaf

/* Number contiguous entries that ARMV8PT_FMT_CONTIG will join at this level */
static inline unsigned short
armv8pt_contig_count_lg2(const struct pt_state *pts)
{
	unsigned int tgsz_lg2 = armv8pt_tgsz_lg2(pts);

	if (tgsz_lg2 == SZLG2_4K)
		return ilog2(16); /* 64KB, 32MB */
	else if (tgsz_lg2 == SZLG2_16K && pts->level == ARML2)
		return ilog2(32); /* 1GB */
	else if (tgsz_lg2 == SZLG2_16K && pts->level == ARML3)
		return ilog2(128); /* 2M */
	else if (tgsz_lg2 == SZLG2_64K)
		return ilog2(32); /* 2M, 16G */
	return ilog2(1);
}
#define pt_contig_count_lg2 armv8pt_contig_count_lg2

static inline unsigned int
armv8pt_entry_num_contig_lg2(const struct pt_state *pts)
{
	if (pts->entry & ARMV8PT_FMT_CONTIG)
		return armv8pt_contig_count_lg2(pts);
	return ilog2(1);
}
#define pt_entry_num_contig_lg2 armv8pt_entry_num_contig_lg2

static inline pt_vaddr_t armv8pt_full_va_prefix(const struct pt_common *common)
{
	if (pt_feature(common, PT_FEAT_ARMV8_TTBR1))
		return PT_VADDR_MAX;
	return 0;
}
#define pt_full_va_prefix armv8pt_full_va_prefix

static inline unsigned int armv8pt_num_items_lg2(const struct pt_state *pts)
{
	/*
	 * It is not allowed to call pt_num_items_lg2() at the top level, this
	 * API restriction is specifically an optimization avoid overheads
	 * dealing with concatenated tables here.
	 */
	PT_WARN_ON(pts->level == pts->range->top_level);
	return armv8pt_tgsz_lg2(pts) - ilog2(sizeof(u64));
}
#define pt_num_items_lg2 armv8pt_num_items_lg2

static inline enum pt_entry_type armv8pt_load_entry_raw(struct pt_state *pts)
{
	const u64 *tablep = pt_cur_table(pts, u64) + pts->index;
	u64 entry;

	pts->entry = entry = READ_ONCE(*tablep);
	if (!(entry & ARMV8PT_FMT_VALID))
		return PT_ENTRY_EMPTY;
	/* R_RWMFF/Table D8-48: ARM level 3 has only Page descriptors */
	if (pts->level != ARML3 && (entry & ARMV8PT_FMT_TABLE))
		return PT_ENTRY_TABLE;

	/* Must be a block or page, don't check the page bit on level 0 */
	return PT_ENTRY_OA;
}
#define pt_load_entry_raw armv8pt_load_entry_raw

static inline void
armv8pt_install_leaf_entry(struct pt_state *pts, pt_oaddr_t oa,
			   unsigned int oasz_lg2,
			   const struct pt_write_attrs *attrs)
{
	unsigned int isz_lg2 = pt_table_item_lg2sz(pts);
	u64 *tablep = pt_cur_table(pts, u64);
	u64 entry;

	if (!pt_check_install_leaf_args(pts, oa, oasz_lg2))
		return;

	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2))
		entry = ARMV8PT_FMT_VALID |
			(oa & GENMASK_ULL(49, armv8pt_tgsz_lg2(pts))) |
			FIELD_PREP(ARMV8PT_FMT_OA52_LPA2, oa >> 50) |
			attrs->descriptor_bits;
	else {
		entry = ARMV8PT_FMT_VALID |
			(oa & GENMASK_ULL(47, armv8pt_tgsz_lg2(pts))) |
			attrs->descriptor_bits;
		/* 64K FEAT_LPA: OA[51:48] in [15:12] */
		if (pts_feature(pts, PT_FEAT_ARMV8_LPA))
			entry |= FIELD_PREP(ARMV8PT_FMT_OA52_LPA, oa >> 48);
	}

	/*
	 * R_RWMFF/Table D8-48: at ARM level 3 the leaf is a Page descriptor
	 * with bit[1]=1; at other levels it is a Block with bit[1]=0.
	 */
	if (pts->level == ARML3)
		entry |= ARMV8PT_FMT_PAGE;

	if (oasz_lg2 != isz_lg2) {
		u64 *end;

		entry |= ARMV8PT_FMT_CONTIG;
		tablep += pts->index;
		end = tablep + log2_to_int(armv8pt_contig_count_lg2(pts));
		for (; tablep != end; tablep++) {
			WRITE_ONCE(*tablep, entry);
			entry += (u64)1 << isz_lg2;
		}
	} else {
		WRITE_ONCE(tablep[pts->index], entry);
	}
	pts->entry = entry;
}
#define pt_install_leaf_entry armv8pt_install_leaf_entry

static inline bool armv8pt_install_table(struct pt_state *pts,
					 pt_oaddr_t table_pa,
					 const struct pt_write_attrs *attrs)
{
	u64 entry;

	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2))
		entry = ARMV8PT_FMT_VALID | ARMV8PT_FMT_TABLE |
			(table_pa & GENMASK_ULL(49, armv8pt_tgsz_lg2(pts))) |
			FIELD_PREP(ARMV8PT_FMT_OA52_LPA2, table_pa >> 50);
	else {
		entry = ARMV8PT_FMT_VALID | ARMV8PT_FMT_TABLE |
			(table_pa & GENMASK_ULL(47, armv8pt_tgsz_lg2(pts)));
		/* 64K FEAT_LPA: OA[51:48] in [15:12] */
		if (pts_feature(pts, PT_FEAT_ARMV8_LPA))
			entry |= FIELD_PREP(ARMV8PT_FMT_OA52_LPA,
					    table_pa >> 48);
	}

	return pt_table_install64(pts, entry);
}
#define pt_install_table armv8pt_install_table

static inline void armv8pt_attr_from_entry(const struct pt_state *pts,
					   struct pt_write_attrs *attrs)
{
	u64 mask = ARMV8PT_FMT_AF | ARMV8PT_FMT_UXN | ARMV8PT_FMT_PXN |
		   ARMV8PT_FMT_ATTRINDX | ARMV8PT_FMT_AP | ARMV8PT_FMT_nG |
		   ARMV8PT_FMT_S2MEMATTR | ARMV8PT_FMT_S2AP;

	/* Tables D8-52/53: with LPA2 bits [9:8] are OA[51:50], not SH */
	if (!pts_feature(pts, PT_FEAT_ARMV8_LPA2))
		mask |= ARMV8PT_FMT_SH;
	attrs->descriptor_bits = pts->entry & mask;
}
#define pt_attr_from_entry armv8pt_attr_from_entry

static inline void armv8pt_clear_entries(struct pt_state *pts,
					 unsigned int num_contig_lg2)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;
	u64 *end = tablep + log2_to_int(num_contig_lg2);

	for (; tablep != end; tablep++)
		WRITE_ONCE(*tablep, 0);
}
#define pt_clear_entries armv8pt_clear_entries

static inline unsigned int armv8pt_max_sw_bit(struct pt_common *common)
{
	/*
	 * Stage 2 bit [55] is the NS field in Realm state (Table D8-53),
	 * so it cannot be used as a software bit for stage 2.
	 */
	if (pt_feature(common, PT_FEAT_ARMV8_S2))
		return 2;
	return 3;
}
#define pt_max_sw_bit armv8pt_max_sw_bit

static inline u64 armv8pt_sw_bit(unsigned int bitnr)
{
	if (__builtin_constant_p(bitnr) && bitnr > 3)
		BUILD_BUG();

	/*
	 * D8.3 Tables D8-50 through D8-53: Bits marked IGNORED in Table
	 * descriptors and "Reserved for software use" in Block and Page
	 * descriptors.
	 *
	 * Bits [58:56] are safe for all descriptor types at both stage 1
	 * and stage 2.  Bit [55] is additionally available at stage 1
	 * (Table D8-52).
	 */
	switch (bitnr) {
	case 0 ... 2:
		return BIT_ULL(56 + bitnr);
	case 3:
		return BIT_ULL(55);
	default:
		PT_WARN_ON(true);
		return 0;
	}
}
#define pt_sw_bit armv8pt_sw_bit
