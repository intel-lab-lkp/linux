/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_TABLE_API_H
#define _ASM_X86_CPUID_TABLE_API_H

#include <asm/cpuid/internal_api.h>
#include <asm/cpuid/types.h>
#include <asm/processor.h>

/*
 * Accessors of scanned CPUID data, intended for external use:
 *
 * Note, all the accessors below require @_leaf and @_subleaf as literals.
 * For example:
 *
 *		cpudata_cpuid(c, 0x0);
 *		cpudata_cpuid_subleaf(c, 0x7, 1);
 *
 * This is due to the CPP tokenization used to construct the CPUID tables
 * at <cpuid/types.h>.
 */

/**
 * cpudata_cpuid_subleaf() - Get scanned CPUID data
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, in 0xN format
 * @_subleaf:	CPUID subleaf, in decimal format
 *
 * Return scanned CPUID output in a ready-to-parse <cpuid/leaves.h> type:
 * 'struct leaf_0xN_M', where 0xN is the leaf token from @_leaf, and M is
 * the subleaf token from @_subleaf.
 *
 * Return NULL if the leaf/subleaf is not present in the scanned table
 * referenced by @_cpuinfo.  This may occur if the leaf is beyond the CPU's
 * max supported standard/extended leaf, or if the CPUID scanner code
 * skipped the @_leaf entry because it was considered invalid.
 */
#define cpudata_cpuid_subleaf(_cpuinfo, _leaf, _subleaf)		\
	__cpudata_cpuid_subleaf(&_cpuinfo->cpuid_table, _leaf, _subleaf)

/**
 * cpudata_cpuid() - Get scanned CPUID data (subleaf = 0)
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, in 0xN format
 *
 * Shortcut for cpudata_cpuid_subleaf() with subleaf = 0.
 */
#define cpudata_cpuid(_cpuinfo, _leaf)					\
	__cpudata_cpuid_subleaf(&_cpuinfo->cpuid_table, _leaf, 0)

/**
 * cpudata_cpuid_nr_entries() - Get Number of filled entries for @_leaf
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, in 0xN format
 *
 * CPUID leaves that enumerate hierarchical structures (e.g. cache topology
 * with leaf 0x4, XSAVE with 0xd, SGX with 0x12) can have multiple valid
 * subleafs with identical output formats. The scanned CPUID table stores
 * these in an output storage array.
 *
 * Return the number of entries filled by the CPUID scanner for @_leaf.
 */
#define cpudata_cpuid_nr_entries(_cpuinfo, _leaf)			\
	__cpuid_info_get(&_cpuinfo->cpuid_table.leaves, _leaf, 0).nr_entries

/**
 * cpudata_cpuid_index() - Get scanned CPUID data at index
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, in 0xN format
 * @_idx:	Index within leaf 0xN output storage array. It must be smaller
 *		than cpudata_cpuid_nr_entries(@_cpuinfo, @_leaf).
 *
 * Similar to cpudata_cpuid(), but accesses a specific indexed entry.  This is
 * useful for CPUID leaves with identical output format for multiple subleaves.
 * For example, accessing CPUID leaf 0x4 output can be done as::
 *
 *	for (int i = 0; i < cpudata_cpuid_nr_entries(c, 0x4); i++) {
 *		const struct leaf_0x4_0 *l4 = cpudata_cpuid_index(c, 0x4, i);
 *		if (!l4)
 *			break;
 *
 *		// Access l4->cache_type, etc.
 *	}
 *
 * Beside the "return NULL" situations detailed at cpudata_cpuid_subleaf(),
 * NULL will also be returned if @_idx is out of range.
 *
 * See 'struct cpuid_leaves' at <asm/cpuid/types.h> for multi-entry leaves.
 * Such leaves will have a CPUID_LEAF() @_count parameter bigger than one.
 */
#define cpudata_cpuid_index(_cpuinfo, _leaf, _idx)			\
	__cpudata_cpuid_subleaf_idx(&_cpuinfo->cpuid_table, _leaf, 0, _idx)

/**
 * cpudata_cpuid_regs() - Get raw register output for scanned CPUID leaf
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, 0xN format
 *
 * Similar to cpudata_cpuid(), but returns a raw 'struct cpuid_regs *' instead
 * of a <cpuid/leaves.h> data type.
 */
#define cpudata_cpuid_regs(_cpuinfo, _leaf)				\
	(struct cpuid_regs *)(cpudata_cpuid(_cpuinfo, _leaf))

/**
 * cpudata_cpuid_index_regs() - Get raw scanned CPUID register output
 * @_cpuinfo:	CPU capability table (struct cpuinfo_x86)
 * @_leaf:	CPUID leaf, in 0xN format
 * @_idx:	Index within leaf 0xN output storage entry. It must be smaller
 *		than cpudata_cpuid_nr_entries(@_cpuinfo, @_leaf).
 *
 * Like cpudata_cpuid_index(), but returns a raw 'struct cpuid_regs *' instead
 * of a <cpuid/leaves.h> data type.
 */
#define cpudata_cpuid_index_regs(_cpuinfo, _leaf, _idx)			\
	(struct cpuid_regs *)cpudata_cpuid_index(_cpuinfo, _leaf, _idx)

void cpuid_scan_cpu(struct cpuinfo_x86 *c);

#endif /* _ASM_X86_CPUID_TABLE_API_H */
