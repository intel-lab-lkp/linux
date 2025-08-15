/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_TYPES_H
#define _ASM_X86_CPUID_TYPES_H

#include <linux/build_bug.h>
#include <linux/types.h>

#include <asm/cpuid/leaf_types.h>

/*
 * Types for raw CPUID access:
 */

struct cpuid_regs {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
};

enum cpuid_regs_idx {
	CPUID_EAX = 0,
	CPUID_EBX,
	CPUID_ECX,
	CPUID_EDX,
};

#define CPUID_LEAF_MWAIT	0x05
#define CPUID_LEAF_DCA		0x09
#define CPUID_LEAF_XSTATE	0x0d
#define CPUID_LEAF_TSC		0x15
#define CPUID_LEAF_FREQ		0x16
#define CPUID_LEAF_TILE		0x1d

#define CPUID_BASE_START	0x0
#define CPUID_EXT_START		0x80000000

#define __CPUID_RANGE_END(idx)	((idx) + 0xffff)
#define CPUID_BASE_END		__CPUID_RANGE_END(CPUID_BASE_START)
#define CPUID_EXT_END		__CPUID_RANGE_END(CPUID_EXT_START)

/*
 * Types for CPUID(0x2) parsing:
 */

struct leaf_0x2_reg {
		u32		: 31,
			invalid	: 1;
};

union leaf_0x2_regs {
	struct leaf_0x2_reg	reg[4];
	u32			regv[4];
	u8			desc[16];
};

/*
 * Leaf 0x2 1-byte descriptors' cache types
 * To be used for their mappings at cpuid_0x2_table[]
 *
 * Start at 1 since type 0 is reserved for HW byte descriptors which are
 * not recognized by the kernel; i.e., those without an explicit mapping.
 */
enum _cache_table_type {
	CACHE_L1_INST		= 1,
	CACHE_L1_DATA,
	CACHE_L2,
	CACHE_L3
	/* Adjust __TLB_TABLE_TYPE_BEGIN before adding more types */
} __packed;
#ifndef __CHECKER__
static_assert(sizeof(enum _cache_table_type) == 1);
#endif

/*
 * Ensure that leaf 0x2 cache and TLB type values do not intersect,
 * since they share the same type field at struct cpuid_0x2_table.
 */
#define __TLB_TABLE_TYPE_BEGIN		(CACHE_L3 + 1)

/*
 * Leaf 0x2 1-byte descriptors' TLB types
 * To be used for their mappings at cpuid_0x2_table[]
 */
enum _tlb_table_type {
	TLB_INST_4K		= __TLB_TABLE_TYPE_BEGIN,
	TLB_INST_4M,
	TLB_INST_2M_4M,
	TLB_INST_ALL,

	TLB_DATA_4K,
	TLB_DATA_4M,
	TLB_DATA_2M_4M,
	TLB_DATA_4K_4M,
	TLB_DATA_1G,
	TLB_DATA_1G_2M_4M,

	TLB_DATA0_4K,
	TLB_DATA0_4M,
	TLB_DATA0_2M_4M,

	STLB_4K,
	STLB_4K_2M,
} __packed;
#ifndef __CHECKER__
static_assert(sizeof(enum _tlb_table_type) == 1);
#endif

/*
 * Combined parsing table for leaf 0x2 cache and TLB descriptors.
 */

struct leaf_0x2_table {
	union {
		enum _cache_table_type	c_type;
		enum _tlb_table_type	t_type;
	};
	union {
		short			c_size;
		short			entries;
	};
};

extern const struct leaf_0x2_table cpuid_0x2_table[256];

/*
 * All of leaf 0x2's one-byte TLB descriptors implies the same number of entries
 * for their respective TLB types.  TLB descriptor 0x63 is an exception: it
 * implies 4 dTLB entries for 1GB pages and 32 dTLB entries for 2MB or 4MB pages.
 *
 * Encode that descriptor's dTLB entry count for 2MB/4MB pages here, as the entry
 * count for dTLB 1GB pages is already encoded at the cpuid_0x2_table[]'s mapping.
 */
#define TLB_0x63_2M_4M_ENTRIES		32

/*
 * Types for centralized CPUID tables:
 *
 * For internal use by the CPUID parser.
 */

/**
 * struct leaf_query_info - Parse info for a CPUID leaf/subleaf query
 * @nr_entries:	Number of valid output storage entries filled by the CPUID parser
 *
 * In a CPUID table (struct cpuid_leaves), each CPUID leaf/subleaf query output
 * storage entry from <cpuid/leaf_types.h> is paired with a unique instance of
 * this type.
 */
struct leaf_query_info {
	unsigned int		nr_entries;
};

/**
 * __CPUID_LEAF() - Define CPUID output storage and query info entry
 * @_name:	Struct type name of the CPUID leaf/subleaf (e.g. 'leaf_0x4_0').
 *		Such types are defined at <cpuid/leaf_types.h>, and follow the
 *		format 'struct leaf_0xN_M', where 0xN is the leaf and M is the
 *		subleaf.
 * @_count:	Number of storage entries to allocate for this leaf/subleaf
 *
 * For the given leaf/subleaf combination, define an array of CPUID output
 * storage entries and an associated query info structure — both residing in a
 * 'struct cpuid_leaves' instance.
 *
 * Use an array of storage entries to accommodate CPUID leaves which produce
 * the same output format for a large subleaf range.  This is common for
 * hierarchical objects enumeration; e.g., CPUID(0x4), CPUID(0xd), and
 * CPUID(0x12).
 *
 * The example invocation for CPUID(0x7) storage, subleaves 0->1:
 *
 *	__CPUID_LEAF(leaf_0x7_0, 1);
 *	__CPUID_LEAF(leaf_0x7_1, 1);
 *
 * generates 'struct cpuid_leaves' storage entries in the form::
 *
 *	struct leaf_0x7_0		leaf_0x7_0[1];
 *	struct leaf_query_info		leaf_0x7_0_info;
 *
 *	struct leaf_0x7_1		leaf_0x7_1[1];
 *	struct leaf_query_info		leaf_0x7_1_info;
 *
 * The example invocation for CPUID(0x4) storage::
 *
 *	__CPUID_LEAF(leaf_0x4_0, 8);
 *
 * generates storage entries in the form:
 *
 *	struct leaf_0x4_0		leaf_0x4_0[8];
 *	struct leaf_query_info		leaf_0x4_0_info;
 *
 * where the 'leaf_0x4_0[8]' storage array can accommodate the output of
 * CPUID(0x4) subleaves 0->7, since they all have the same output format.
 */
#define __CPUID_LEAF(_name, _count)				\
	struct _name		_name[_count];			\
	struct leaf_query_info	_name##_info

/**
 * CPUID_LEAF() - Define a CPUID storage entry in 'struct cpuid_leaves'
 * @_leaf:	CPUID Leaf number in the 0xN format; e.g., 0x4.
 * @_subleaf:	Subleaf number in decimal
 * @_count:	Number of repeated storage entries for this @_leaf/@_subleaf
 *
 * Convenience wrapper around __CPUID_LEAF().
 */
#define CPUID_LEAF(_leaf, _subleaf, _count)			\
	__CPUID_LEAF(leaf_ ## _leaf ## _ ## _subleaf, _count)

/*
 * struct cpuid_leaves - Structured CPUID data repository
 */
struct cpuid_leaves {
	/*         leaf		subleaf		count */
	CPUID_LEAF(0x0,		0,		1);
	CPUID_LEAF(0x1,		0,		1);
	CPUID_LEAF(0x80000000,	0,		1);
};

/*
 * Types for centralized CPUID tables:
 *
 * For external use.
 */

/**
 * struct cpuid_table - Per-CPU CPUID data repository
 * @leaves:	CPUID leaf/subleaf queries' output and metadata
 *
 * Embedded inside 'struct cpuinfo_x86' to provide access to stored, parsed,
 * and sanitized CPUID output per-CPU.  Thus removing the need for any direct
 * CPUID query by call sites.
 */
struct cpuid_table {
	struct cpuid_leaves	leaves;
};

#endif /* _ASM_X86_CPUID_TYPES_H */
