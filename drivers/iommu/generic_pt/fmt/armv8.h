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
 *   generic_pt | ARM   | 4K coverage | 16K coverage | 64K coverage
 *   -----------+-------+-------------+--------------+-------------
 *   0          |  3    | 4KB page    | 16KB page    | 64KB page
 *   1          |  2    | 2MB         | 32MB         | 512MB
 *   2          |  1    | 1GB         | 64GB         | 4TB
 *   3          |  0    | 512GB       | 128TB        | -
 *   4          | -1    | 256TB       | -            | -
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

	ARMV8PT_FMT_DBM = BIT_ULL(51),
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
static inline unsigned int armv8pt_granule_lg2sz(const struct pt_state *pts)
{
	return to_armv8pt(pts)->granule_lg2sz;
}

static inline u64 armv8pt_fmt_oa48(unsigned int granule_lg2sz)
{
	return GENMASK_ULL(47, granule_lg2sz);
}

/*
 * The generic implementations cannot be used if PT_GRANULE_LG2SZ is dynamic.
 */
static inline unsigned int armv8pt_table_item_lg2sz(const struct pt_state *pts)
{
	unsigned int granule_lg2sz = armv8pt_granule_lg2sz(pts);

	return granule_lg2sz +
	       (granule_lg2sz - ilog2(PT_ITEM_WORD_SIZE)) * pts->level;
}
#define pt_table_item_lg2sz armv8pt_table_item_lg2sz

static inline unsigned int armv8pt_pgsz_lg2_to_level(struct pt_common *common,
						     unsigned int pgsize_lg2)
{
	unsigned int granule_lg2sz = common_to_armv8pt(common)->granule_lg2sz;

	return (pgsize_lg2 - granule_lg2sz) /
	       (granule_lg2sz - ilog2(sizeof(u64)));
}
#define pt_pgsz_lg2_to_level armv8pt_pgsz_lg2_to_level

static inline bool armv8pt_has_system_page_size(const struct pt_common *common)
{
	return common_to_armv8pt(common)->granule_lg2sz == PAGE_SHIFT;
}
#define pt_has_system_page_size armv8pt_has_system_page_size

static inline pt_oaddr_t armv8pt_oa(const struct pt_state *pts)
{
	u64 entry = pts->entry;
	unsigned int granule_lg2sz = armv8pt_granule_lg2sz(pts);
	pt_oaddr_t oa;

	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2)) {
		/* 4K/16K with DS=1: OA[49:granule] direct, OA[51:50] in [9:8] */
		oa = entry & GENMASK_ULL(49, granule_lg2sz);
		oa |= ((pt_oaddr_t)FIELD_GET(ARMV8PT_FMT_OA52_LPA2, entry))
		      << 50;
	} else {
		oa = entry & GENMASK_ULL(47, granule_lg2sz);
		if (PT_SUPPORTED_FEATURE(PT_FEAT_ARMV8_LPA) &&
		    granule_lg2sz == SZLG2_64K)
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
	unsigned int granule_lg2sz = armv8pt_granule_lg2sz(pts);

	/*
	 * Tables D8-16/17 (4K), D8-26/27 (16K), D8-35/36 (64K).
	 *
	 * When FEAT_LPA2, one additional level gains block descriptors:
	 *   4K:  ARM level 0 (pt 3) gains 512GB blocks
	 *   16K: ARM level 1 (pt 2) gains 64GB blocks
	 * ARM Level -1 (pt 4) is always table-only.
	 */
	if (pts_feature(pts, PT_FEAT_ARMV8_LPA2)) {
		if (granule_lg2sz == SZLG2_4K && pts->level > ARML0)
			return false;
		if (granule_lg2sz == SZLG2_16K && pts->level > ARML1)
			return false;
		if (granule_lg2sz == SZLG2_64K && pts->level > ARML2)
			return false;
	} else {
		if (granule_lg2sz == SZLG2_4K && pts->level > ARML1)
			return false;
		if (granule_lg2sz == SZLG2_16K && pts->level > ARML2)
			return false;
		if (granule_lg2sz == SZLG2_64K &&
		    pts->level > (pts_feature(pts, PT_FEAT_ARMV8_LPA) ?
				  ARML1 : ARML2))
			return false;
	}
	return true;
}
#define pt_can_have_leaf armv8pt_can_have_leaf

/* Number contiguous entries that ARMV8PT_FMT_CONTIG will join at this level */
static inline unsigned short
armv8pt_contig_count_lg2(const struct pt_state *pts)
{
	unsigned int granule_lg2sz = armv8pt_granule_lg2sz(pts);

	if (granule_lg2sz == SZLG2_4K)
		return ilog2(16); /* 64KB, 32MB */
	else if (granule_lg2sz == SZLG2_16K && pts->level == ARML2)
		return ilog2(32); /* 1GB */
	else if (granule_lg2sz == SZLG2_16K && pts->level == ARML3)
		return ilog2(128); /* 2M */
	else if (granule_lg2sz == SZLG2_64K)
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
	/* It is not allowed to call pt_num_items_lg2() at the top level */
	PT_WARN_ON(pts->level == pts->range->top_level);
	return armv8pt_granule_lg2sz(pts) - ilog2(sizeof(u64));
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

	/*
	 * Suppress returning OA for levels that cannot have a page to remove
	 * code.
	 */
	if (!armv8pt_can_have_leaf(pts))
		return PT_ENTRY_EMPTY;

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
			(oa & GENMASK_ULL(49, armv8pt_granule_lg2sz(pts))) |
			FIELD_PREP(ARMV8PT_FMT_OA52_LPA2, oa >> 50) |
			attrs->descriptor_bits;
	else {
		entry = ARMV8PT_FMT_VALID |
			(oa & GENMASK_ULL(47, armv8pt_granule_lg2sz(pts))) |
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
			(table_pa &
			 GENMASK_ULL(49, armv8pt_granule_lg2sz(pts))) |
			FIELD_PREP(ARMV8PT_FMT_OA52_LPA2, table_pa >> 50);
	else {
		entry = ARMV8PT_FMT_VALID | ARMV8PT_FMT_TABLE |
			(table_pa &
			 GENMASK_ULL(47, armv8pt_granule_lg2sz(pts)));
		/* 64K FEAT_LPA: OA[51:48] in [15:12] */
		if (pts_feature(pts, PT_FEAT_ARMV8_LPA))
			entry |= FIELD_PREP(ARMV8PT_FMT_OA52_LPA,
					    table_pa >> 48);
	}

	if (pts_feature(pts, PT_FEAT_ARMV8_NS))
		entry |= ARMV8PT_FMT_NSTABLE;

	return pt_table_install64(pts, entry);
}
#define pt_install_table armv8pt_install_table

static inline void armv8pt_attr_from_entry(const struct pt_state *pts,
					   struct pt_write_attrs *attrs)
{
	u64 mask = ARMV8PT_FMT_AF | ARMV8PT_FMT_DBM | ARMV8PT_FMT_UXN |
		   ARMV8PT_FMT_PXN | ARMV8PT_FMT_ATTRINDX | ARMV8PT_FMT_AP |
		   ARMV8PT_FMT_nG | ARMV8PT_FMT_S2MEMATTR | ARMV8PT_FMT_S2AP;

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

	if (num_contig_lg2 == 0)
		WRITE_ONCE(*tablep, 0);
	else
		memset64(tablep, 0, log2_to_int(num_contig_lg2));
}
#define pt_clear_entries armv8pt_clear_entries

/*
 * Call fn over all the items in an entry. If the entry is contiguous this
 * iterates over the entire contiguous entry, including items preceding
 * pts->va. always_inline avoids an indirect function call.
 */
static __always_inline bool armv8pt_reduce_contig(const struct pt_state *pts,
						  bool (*fn)(u64 *tablep,
							     u64 entry))
{
	u64 *tablep = pt_cur_table(pts, u64);

	if (pts->entry & ARMV8PT_FMT_CONTIG) {
		unsigned int num_contig_lg2 = armv8pt_contig_count_lg2(pts);
		u64 *end;

		tablep += log2_set_mod(pts->index, 0, num_contig_lg2);
		end = tablep + log2_to_int(num_contig_lg2);
		for (; tablep != end; tablep++)
			if (fn(tablep, READ_ONCE(*tablep)))
				return true;
		return false;
	}
	return fn(tablep + pts->index, pts->entry);
}

static inline bool armv8pt_check_is_dirty_s1(u64 *tablep, u64 entry)
{
	return (entry & (ARMV8PT_FMT_DBM |
			 FIELD_PREP(ARMV8PT_FMT_AP, ARMV8PT_AP_RDONLY))) ==
	       ARMV8PT_FMT_DBM;
}

static bool armv6pt_clear_dirty_s1(u64 *tablep, u64 entry)
{
	WRITE_ONCE(*tablep,
		   entry | FIELD_PREP(ARMV8PT_FMT_AP, ARMV8PT_AP_RDONLY));
	return false;
}

static inline bool armv8pt_check_is_dirty_s2(u64 *tablep, u64 entry)
{
	const u64 DIRTY = ARMV8PT_FMT_DBM |
			  FIELD_PREP(ARMV8PT_FMT_S2AP, ARMV8PT_S2AP_WRITE);

	return (entry & DIRTY) == DIRTY;
}

static bool armv6pt_clear_dirty_s2(u64 *tablep, u64 entry)
{
	WRITE_ONCE(*tablep, entry & ~(u64)FIELD_PREP(ARMV8PT_FMT_S2AP,
						     ARMV8PT_S2AP_WRITE));
	return false;
}

static inline bool armv8pt_entry_is_write_dirty(const struct pt_state *pts)
{
	if (!pts_feature(pts, PT_FEAT_ARMV8_S2))
		return armv8pt_reduce_contig(pts, armv8pt_check_is_dirty_s1);
	else
		return armv8pt_reduce_contig(pts, armv8pt_check_is_dirty_s2);
}
#define pt_entry_is_write_dirty armv8pt_entry_is_write_dirty

static inline void armv8pt_entry_make_write_clean(struct pt_state *pts)
{
	if (!pts_feature(pts, PT_FEAT_ARMV8_S2))
		armv8pt_reduce_contig(pts, armv6pt_clear_dirty_s1);
	else
		armv8pt_reduce_contig(pts, armv6pt_clear_dirty_s2);
}
#define pt_entry_make_write_clean armv8pt_entry_make_write_clean

static inline bool armv8pt_entry_make_write_dirty(struct pt_state *pts)
{
	u64 *tablep = pt_cur_table(pts, u64) + pts->index;
	u64 new = pts->entry;

	if (!(pts->entry & ARMV8PT_FMT_DBM))
		return false;

	if (!pts_feature(pts, PT_FEAT_ARMV8_S2)) {
		new &= ~(u64)ARMV8PT_FMT_AP;
		new |= FIELD_PREP(ARMV8PT_FMT_AP, 0);
	} else {
		new &= ~(u64)ARMV8PT_FMT_S2AP;
		new |= FIELD_PREP(ARMV8PT_FMT_S2AP, ARMV8PT_S2AP_WRITE);
	}

	return try_cmpxchg64(tablep, &pts->entry, new);
}
#define pt_entry_make_write_dirty armv8pt_entry_make_write_dirty

static inline bool armv8pt_dirty_supported(struct pt_common *common)
{
	return pt_feature(common, PT_FEAT_ARMV8_DBM);
}
#define pt_dirty_supported armv8pt_dirty_supported

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

/* --- iommu */
#include <linux/generic_pt/iommu.h>
#include <linux/iommu.h>

#define pt_iommu_table pt_iommu_armv8

/* The common struct is in the per-format common struct */
static inline struct pt_common *common_from_iommu(struct pt_iommu *iommu_table)
{
	return &container_of(iommu_table, struct pt_iommu_table, iommu)
			->armpt.common;
}

static inline struct pt_iommu *iommu_from_common(struct pt_common *common)
{
	return &container_of(common, struct pt_iommu_table, armpt.common)->iommu;
}

static inline int armv8pt_iommu_set_prot(struct pt_common *common,
					 struct pt_write_attrs *attrs,
					 unsigned int iommu_prot)
{
	bool is_s1 = !pt_feature(common, PT_FEAT_ARMV8_S2);
	u64 pte = 0;

	if (is_s1) {
		u64 ap = 0;

		if (!(iommu_prot & IOMMU_WRITE) && (iommu_prot & IOMMU_READ))
			ap |= ARMV8PT_AP_RDONLY;
		if (!(iommu_prot & IOMMU_PRIV))
			ap |= ARMV8PT_AP_UNPRIV;
		pte = ARMV8PT_FMT_nG | FIELD_PREP(ARMV8PT_FMT_AP, ap);

		if (iommu_prot & IOMMU_MMIO)
			pte |= FIELD_PREP(ARMV8PT_FMT_ATTRINDX,
					  ARMV8PT_MAIR_ATTR_IDX_DEV);
		else if (iommu_prot & IOMMU_CACHE)
			pte |= FIELD_PREP(ARMV8PT_FMT_ATTRINDX,
					  ARMV8PT_MAIR_ATTR_IDX_CACHE);
		else
			pte |= FIELD_PREP(ARMV8PT_FMT_ATTRINDX,
					  ARMV8PT_MAIR_ATTR_IDX_NC);
	} else {
		u64 s2ap = 0;

		if (iommu_prot & IOMMU_READ)
			s2ap |= ARMV8PT_S2AP_READ;
		if (iommu_prot & IOMMU_WRITE)
			s2ap |= ARMV8PT_S2AP_WRITE;
		pte = FIELD_PREP(ARMV8PT_FMT_S2AP, s2ap);

		if (iommu_prot & IOMMU_MMIO)
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_DEV);
		else if ((iommu_prot & IOMMU_CACHE) &&
			 pt_feature(common, PT_FEAT_ARMV8_S2FWB))
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_FWB_WB);
		else if (iommu_prot & IOMMU_CACHE)
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_OIWB);
		else
			pte |= FIELD_PREP(ARMV8PT_FMT_S2MEMATTR,
					  ARMV8PT_MEMATTR_NC);
	}

	/*
	 * For DBM the writable entry starts out dirty to avoid the HW doing
	 * memory accesses to dirty it. We can just leave the DBM bit
	 * permanently set with no cost.
	 */
	if (pt_feature(common, PT_FEAT_ARMV8_DBM) && (iommu_prot & IOMMU_WRITE))
		pte |= ARMV8PT_FMT_DBM;

	/* Tables D8-52/53: with LPA2 bits [9:8] are OA[51:50], not SH */
	if (!pt_feature(common, PT_FEAT_ARMV8_LPA2)) {
		if (iommu_prot & IOMMU_CACHE)
			pte |= FIELD_PREP(ARMV8PT_FMT_SH, ARMV8PT_SH_IS);
		else
			pte |= FIELD_PREP(ARMV8PT_FMT_SH, ARMV8PT_SH_OS);
	}

	if (iommu_prot & IOMMU_NOEXEC) {
		/*
		 * Assume S2 AArch32 does not have FEAT_XNX. Execute permissions
		 * are very rarely used by the iommu and it doesn't really have
		 * ELs to make use of PXN.
		 */
		pte |= ARMV8PT_FMT_PXN;
		if (!pt_feature(common, PT_FEAT_ARMV8_AARCH32) || is_s1)
			pte |= ARMV8PT_FMT_UXN;
	}

	if (pt_feature(common, PT_FEAT_ARMV8_NS))
		pte |= ARMV8PT_FMT_NS;

	pte |= ARMV8PT_FMT_AF;

	attrs->descriptor_bits = pte;
	return 0;
}
#define pt_iommu_set_prot armv8pt_iommu_set_prot

static inline unsigned int armv8pt_max_top_level(unsigned int granule_lg2sz,
						 unsigned int features)
{
	if (granule_lg2sz == SZLG2_64K)
		return ARML1;
	if (granule_lg2sz == SZLG2_4K && (features & BIT(PT_FEAT_ARMV8_LPA2)))
		return ARMLn1;
	return ARML0;
}

/*
 * Validate the S2 initial lookup level against D8.2.
 *
 *   Table D8-8: "Effective minimum value of T0SZ" (R_DTLMN)
 *   Table D8-9: "Implications of the effective minimum T0SZ value
 *                on the initial stage 2 lookup level" (R_TDJSG)
 *
 * ARM initial lookup level = 3 - top_level. Table D8-9 constrains the
 * shallowest allowed initial lookup level per PA size and granule:
 *
 *   4K:  ARM level 0 (top_level 3) requires PA >= 44
 *   16K: ARM level 1 (top_level 2) requires PA >= 42
 *   64K: ARM level 1 (top_level 2) requires PA >= 44
 *
 * The valid level set grows monotonically with PA size, so checking
 * against IAS (vasz_lg2 <= PA size) is conservative.
 *
 * R_SRKBC: 4K granule at ARM level 3 (single entry level) requires
 * FEAT_TTST.
 */
static inline void armv8pt_s2_validate_level(unsigned int top_level,
					     unsigned int granule_lg2sz,
					     unsigned int vasz_lg2, bool lpa2)
{
	unsigned int max_top_level;

	switch (granule_lg2sz) {
	case SZLG2_4K:
		if (lpa2)
			max_top_level =
				vasz_lg2 >= 52 ?
					ARMLn1 :
					(vasz_lg2 >= 44 ? ARML0 : ARML1);
		else
			max_top_level = vasz_lg2 >= 44 ? ARML0 : ARML1;
		break;
	case SZLG2_16K: /* ARM level 1 requires PA >= 42 */
		max_top_level = vasz_lg2 >= 42 ? ARML1 : ARML2;
		break;
	case SZLG2_64K: /* ARM level 1 requires PA >= 44 */
		max_top_level = vasz_lg2 >= 44 ? ARML1 : ARML2;
		break;
	default:
		return;
	}

	PT_WARN_ON(top_level > max_top_level);
}

static inline int armv8pt_oasz_to_ps(unsigned int oasz_lg2)
{
	/* Stream Table Entry: S2PS section, Context Descriptor: IPS section */
	switch (oasz_lg2) {
	case 32:
		return 0;
	case 36:
		return 1;
	case 40:
		return 2;
	case 42:
		return 3;
	case 44:
		return 4;
	case 48:
		return 5;
	case 52:
		return 6;
	default:
		return -1;
	}
}

static inline int armv8pt_iommu_fmt_init(struct pt_iommu_armv8 *iommu_table,
					 const struct pt_iommu_armv8_cfg *cfg)
{
	struct pt_armv8 *armv8pt = &iommu_table->armpt;
	unsigned int vasz_lg2 = cfg->common.hw_max_vasz_lg2;
	unsigned int oasz_lg2 = cfg->common.hw_max_oasz_lg2;
	unsigned int granule_lg2sz = cfg->granule_lg2sz;
	unsigned int max_top_level;
	unsigned int levels;

	if (granule_lg2sz != SZLG2_4K && granule_lg2sz != SZLG2_16K &&
	    granule_lg2sz != SZLG2_64K)
		return -EOPNOTSUPP;

	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_AARCH32)) {
		/*
		 * VMSAv8-32 Long-descriptor format (G5.5):
		 * Uses the same 64-bit LPAE descriptors as AArch64 but with
		 * tighter constraints on address ranges and granule size.
		 * FEAT_BTI (Guard Page) is not supported either.
		 */

		if (granule_lg2sz != SZLG2_4K)
			return -EOPNOTSUPP;

		if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2)) {
			if (vasz_lg2 > 40)
				return -EOPNOTSUPP;
		} else {
			if (vasz_lg2 > 32)
				return -EOPNOTSUPP;
		}

		if (oasz_lg2 > 40)
			return -EOPNOTSUPP;

		if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA) ||
		    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA2) ||
		    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2FWB) ||
		    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LVA) ||
		    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_DBM))
			return -EOPNOTSUPP;
	}

	armv8pt->granule_lg2sz = granule_lg2sz;
	max_top_level =
		armv8pt_max_top_level(granule_lg2sz, armv8pt->common.features);

	/* The NS quirk doesn't apply at stage 2 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_NS) &&
	    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2))
		return -EOPNOTSUPP;

	/* LPA2 (DS=1) is only valid for 4K and 16K granules */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA2) &&
	    granule_lg2sz == SZLG2_64K)
		return -EOPNOTSUPP;

	/* LPA is only valid for the 64K granule */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA) &&
	    granule_lg2sz != SZLG2_64K)
		return -EOPNOTSUPP;

	/* LVA is only valid for 64K granule Stage 1 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LVA) &&
	    (granule_lg2sz != SZLG2_64K ||
	     pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2)))
		return -EOPNOTSUPP;

	if (vasz_lg2 <= granule_lg2sz)
		return -EINVAL;

	/* R_QQQSJ: Limit the OA to what the format supports */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA2) ||
	    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA))
		armv8pt->common.max_oasz_lg2 = min(52, oasz_lg2);
	else
		armv8pt->common.max_oasz_lg2 = min(48, oasz_lg2);

	if (armv8pt_oasz_to_ps(armv8pt->common.max_oasz_lg2) < 0)
		return -EOPNOTSUPP;

	if (WARN_ON(armv8pt->common.max_oasz_lg2 > PT_MAX_OUTPUT_ADDRESS_LG2))
		return -EOPNOTSUPP;

	/*
	 * Limit the VA/IPA to what the format supports:
	 *  - LPA2: 52-bit VA for 4K/16K (S1 and S2)
	 *  - LVA:  52-bit VA for 64K S1
	 *  - LPA:  52-bit IPA for 64K S2
	 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA2) ||
	    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LVA) ||
	    (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2) &&
	     pt_feature(&armv8pt->common, PT_FEAT_ARMV8_LPA)))
		armv8pt->common.max_vasz_lg2 = min(52, vasz_lg2);
	else
		armv8pt->common.max_vasz_lg2 = min(48, vasz_lg2);
	vasz_lg2 = armv8pt->common.max_vasz_lg2;

	levels = DIV_ROUND_UP(vasz_lg2 - granule_lg2sz,
			      granule_lg2sz - ilog2(sizeof(u64)));
	if (levels > max_top_level + 1)
		return -EINVAL;

	/*
	 * R_SRKBC: For the 4KB granule, an initial lookup level of 3 is
	 * only supported if FEAT_TTST is implemented. See Table D8-9 and
	 * Table D8-24. FEAT_TTST is not supported.
	 */
	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2) &&
	    granule_lg2sz == SZLG2_4K && levels == 1)
		return -EINVAL;

	/*
	 * D8.2.2: Always use the S2 concatenated tables feature (I_TDMHR)
	 * to fold a top level of up to 16 tables into the next lower
	 * level. Since FEAT_TTST is not supported single level cannot be
	 * selected here either. Notice that there are a number of cases
	 * in the spec that require concatenated tables (eg R_DXBSH),
	 * since this always uses them it is OK. See commit 4dcac8407fe1
	 * ("iommu/io-pgtable-arm: Fix stage-2 concatenation with 16K")
	 */
	if (!pt_feature(&armv8pt->common, PT_FEAT_DYNAMIC_TOP) &&
	    pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2) && levels > 1) {
		unsigned int topsz_lg2 =
			vasz_lg2 -
			(granule_lg2sz +
			 (granule_lg2sz - ilog2(sizeof(u64))) * (levels - 1));
		if (topsz_lg2 <= ilog2(16))
			levels--;
	}

	if (pt_feature(&armv8pt->common, PT_FEAT_ARMV8_S2))
		armv8pt_s2_validate_level(levels - 1, granule_lg2sz, vasz_lg2,
					  pt_feature(&armv8pt->common,
						     PT_FEAT_ARMV8_LPA2));
	pt_top_set_level(&armv8pt->common, levels - 1);
	return 0;
}
#define pt_iommu_fmt_init armv8pt_iommu_fmt_init

static inline void
armv8pt_iommu_fmt_hw_info(struct pt_iommu_armv8 *table,
			  const struct pt_range *top_range,
			  struct pt_iommu_armv8_hw_info *info)
{
	struct pt_common *common = &table->armpt.common;
	unsigned int granule_lg2sz = table->armpt.granule_lg2sz;

	info->aa64 = pt_feature(common, PT_FEAT_ARMV8_AARCH32);
#ifdef __BIG_ENDIAN
	info->endi = 1;
#else
	info->endi = 0;
#endif

	info->ttb = virt_to_phys(top_range->top_table);
	WARN_ON(info->ttb & ~PT_TOP_PHYS_MASK);

	/* D24.2.210 T0SZ: The region size is 2^(64-T0SZ) bytes. */
	info->tsz = 64 - common->max_vasz_lg2;

	/*
	 * Context Descriptor TG0/TG1 use different encodings
	 * Stream Table Entry S2TG is the same as TG0
	 */
	if (pt_feature(common, PT_FEAT_ARMV8_TTBR1)) {
		switch (granule_lg2sz) {
		case SZLG2_4K:
			info->tg = 2;
			break;
		case SZLG2_16K:
			info->tg = 1;
			break;
		case SZLG2_64K:
			info->tg = 3;
			break;
		}
	} else {
		switch (granule_lg2sz) {
		case SZLG2_4K:
			info->tg = 0;
			break;
		case SZLG2_16K:
			info->tg = 2;
			break;
		case SZLG2_64K:
			info->tg = 1;
			break;
		}
	}

	info->ps = armv8pt_oasz_to_ps(common->max_oasz_lg2);
	info->ds = pt_feature(common, PT_FEAT_ARMV8_LPA2);

	if (pt_feature(common, PT_FEAT_DMA_INCOHERENT)) {
		info->sh = ARMV8PT_SH_OS;
		info->irgn = ARMV8PT_RGN_NC;
		info->orgn = ARMV8PT_RGN_NC;
	} else {
		info->sh = ARMV8PT_SH_IS;
		info->irgn = ARMV8PT_RGN_WBWA;
		info->orgn = ARMV8PT_RGN_WBWA;
	}

	if (pt_feature(common, PT_FEAT_ARMV8_S2)) {
		/*
		 * STE S2SL0/S2SL2: Starting level in VTCR_EL2.SL0/SL2
		 * encoding. Table from D24.2.210 SL0:
		 *
		 * 4K:  top_level  ARMLx    S2SL0  S2SL2
		 *      1          ARML2    0      0
		 *      2          ARML1    1      0
		 *      3          ARML0    2      0
		 *      4          ARMLn1   0      1      (FEAT_LPA2)
		 *
		 * 16K/64K:
		 *      0          ARML3    0
		 *      1          ARML2    1
		 *      2          ARML1    2
		 */
		info->s2.sl2 = 0;
		if (granule_lg2sz == SZLG2_4K) {
			if (top_range->top_level == ARMLn1) {
				info->s2.sl0 = 0;
				info->s2.sl2 = 1;
			} else {
				info->s2.sl0 = top_range->top_level - 1;
			}
		} else {
			info->s2.sl0 = top_range->top_level;
		}
	} else {
		info->s1.tbix = 0;
		if (pt_feature(common, PT_FEAT_ARMV8_TTBR1)) {
			info->s1.epd0 = 1;
			info->s1.epd1 = 0;
		} else {
			info->s1.epd0 = 0;
			info->s1.epd1 = 1;
		}

		/*
		 * MAIR value for S1 page tables. Matches what io-pgtable-arm
		 * used.
		 */
		info->s1.mair =
			FIELD_PREP(ARMV8PT_MAIR_ITEM
					   << (ARMV8PT_MAIR_ATTR_IDX_NC * 8),
				   ARMV8PT_MAIR_ATTR_NC) |
			FIELD_PREP(ARMV8PT_MAIR_ITEM
					   << (ARMV8PT_MAIR_ATTR_IDX_CACHE * 8),
				   ARMV8PT_MAIR_ATTR_WBRWA) |
			FIELD_PREP(ARMV8PT_MAIR_ITEM
					   << (ARMV8PT_MAIR_ATTR_IDX_DEV * 8),
				   ARMV8PT_MAIR_ATTR_DEVICE) |
			FIELD_PREP(
				ARMV8PT_MAIR_ITEM
					<< (ARMV8PT_MAIR_ATTR_IDX_INC_OCACHE *
					    8),
				ARMV8PT_MAIR_ATTR_INC_OWBRWA);
	}
}
#define pt_iommu_fmt_hw_info armv8pt_iommu_fmt_hw_info

#if defined(GENERIC_PT_KUNIT)
static const struct pt_iommu_armv8_cfg armv8_kunit_fmt_cfgs[] = {
	/* 4K granule */
	[0] = { .granule_lg2sz = 12,
		.common.features = BIT(PT_FEAT_ARMV8_DBM),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[1] = { .granule_lg2sz = 12,
		.common.features = BIT(PT_FEAT_ARMV8_NS),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[2] = { .granule_lg2sz = 12,
		.common.features = BIT(PT_FEAT_ARMV8_S2),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[3] = { .granule_lg2sz = 12,
		.common.features = BIT(PT_FEAT_ARMV8_TTBR1),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	/* 16K granule */
	[4] = { .granule_lg2sz = 14,
		.common.features = BIT(PT_FEAT_ARMV8_DBM),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[5] = { .granule_lg2sz = 14,
		.common.features = BIT(PT_FEAT_ARMV8_NS),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	/*
	 * See R_DXBSH: 16K granule + 48-bit S2 is required to start at level 1
	 * with 2 concatenated tables.
	 */
	[6] = { .granule_lg2sz = 14,
		.common.features = BIT(PT_FEAT_ARMV8_S2),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[7] = { .granule_lg2sz = 14,
		.common.features = BIT(PT_FEAT_ARMV8_TTBR1),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	/* 64K granule */
	[8] = { .granule_lg2sz = 16,
		.common.features = BIT(PT_FEAT_ARMV8_DBM),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[9] = { .granule_lg2sz = 16,
		.common.features = BIT(PT_FEAT_ARMV8_NS),
		.common.hw_max_oasz_lg2 = 48,
		.common.hw_max_vasz_lg2 = 48 },
	[10] = { .granule_lg2sz = 16,
		 .common.features = BIT(PT_FEAT_ARMV8_S2),
		 .common.hw_max_oasz_lg2 = 48,
		 .common.hw_max_vasz_lg2 = 48 },
	[11] = { .granule_lg2sz = 16,
		 .common.features = BIT(PT_FEAT_ARMV8_TTBR1),
		 .common.hw_max_oasz_lg2 = 48,
		 .common.hw_max_vasz_lg2 = 48 },
	/* S2 concatenated table configurations at smaller IPA sizes */
	[12] = { .granule_lg2sz = 12,
		 .common.features = BIT(PT_FEAT_ARMV8_S2),
		 .common.hw_max_oasz_lg2 = 40,
		 .common.hw_max_vasz_lg2 = 40 },
	[13] = { .granule_lg2sz = 12,
		 .common.features = BIT(PT_FEAT_ARMV8_S2),
		 .common.hw_max_oasz_lg2 = 42,
		 .common.hw_max_vasz_lg2 = 42 },
	[14] = { .granule_lg2sz = 14,
		 .common.features = BIT(PT_FEAT_ARMV8_S2),
		 .common.hw_max_oasz_lg2 = 40,
		 .common.hw_max_vasz_lg2 = 40 },
};
#define kunit_fmt_cfgs armv8_kunit_fmt_cfgs
enum {
	KUNIT_FMT_FEATURES = BIT(PT_FEAT_ARMV8_TTBR1) | BIT(PT_FEAT_ARMV8_S2) |
			     BIT(PT_FEAT_ARMV8_DBM) | BIT(PT_FEAT_ARMV8_S2FWB) |
			     BIT(PT_FEAT_ARMV8_NS) | BIT(PT_FEAT_DYNAMIC_TOP) |
			     BIT(PT_FEAT_ARMV8_LPA) | BIT(PT_FEAT_ARMV8_LPA2) |
			     BIT(PT_FEAT_ARMV8_LVA) | BIT(PT_FEAT_ARMV8_AARCH32)
};
#endif
#endif
