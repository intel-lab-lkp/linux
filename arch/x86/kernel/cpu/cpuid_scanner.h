/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_X86_CPUID_SCANNER_H
#define _ARCH_X86_CPUID_SCANNER_H

#include <asm/cpuid/types.h>

/*
 * struct cpuid_leaves leaf output and leaf query info accessors:
 * @_leaf:	Leaf number in the 0xN format
 * @_subleaf:	Subleaf number in decimal
 *
 * Accessing a leaf and its metadata requires compile-time tokenization, so
 * divide the CPUID scanning logic into two steps: macros and generic runtime
 * code.  The output of the macros, __cpuid_leaves_*_offset(), will be cached
 * by a compile-time "scan entry" then passed to the cpuid_leaves_*_p()
 * inline functions.
 */

#define __cpuid_leaves_leaf_offset(_leaf, _subleaf)			\
	offsetof(struct cpuid_leaves, leaf_ ## _leaf ## _ ## _subleaf)

#define __cpuid_leaves_info_offset(_leaf, _subleaf)			\
	offsetof(struct cpuid_leaves, leaf_ ## _leaf ## _ ## _subleaf ## _ ## info)

#define __cpuid_leaves_leaf_maxcnt(_leaf, _subleaf)			\
	ARRAY_SIZE(((struct cpuid_leaves *)NULL)->leaf_ ## _leaf ## _ ## _subleaf)

static inline struct cpuid_regs *
cpuid_leaves_leaf_p(const struct cpuid_leaves *leaves, unsigned long leaf_offset)
{
	return (struct cpuid_regs *)((unsigned long)(leaves) + leaf_offset);
}

static inline struct leaf_query_info *
cpuid_leaves_info_p(const struct cpuid_leaves *leaves, unsigned long info_offset)
{
	return (struct leaf_query_info *)((unsigned long)(leaves) + info_offset);
}

/**
 * struct cpuid_read_output - Output of a CPUID read operation
 * @leaf:	Pointer to an array of registers; for saving read CPUID data
 * @info:	Pointer to query info; for saving number of filled @leaf entries
 *
 * A CPUID read function such as cpuid_read_generic() or cpuid_read_0xN() uses this
 * structure for output.  Storage for @leaf and @info is provided by the CPUID read
 * function caller, and is typically within a CPUID repo (&struct cpuid_table.leaves).
 */
struct cpuid_read_output {
	struct cpuid_regs	*leaf;
	struct leaf_query_info	*info;
};

/**
 * struct cpuid_scan_entry - Scan info for @leaf/@subleaf
 * @leaf:	Leaf number to be scanned
 * @subleaf:	Subleaf number to be scanned
 * @leaf_offs:	struct cpuid_leaves entry offset for @leaf/@subleaf; to be passed to cpuid_leaves_leaf_p()
 * @info_offs:	struct cpuid_leaves entry offset for @leaf/@subleaf scan info; to be passed to cpuid_leaves_info_p()
 * @maxcnt:	Maximum number of storage entries available for a @leaf/@subleaf query
 * @read:	Read function for this entry.  It must save the read CPUID data to the passed
 *		struct cpuid_read_output.leaf register array of size >= @maxcnt.  It must also
 *		set struct cpuid_read_output.info.nr_entries to the number of entries filled.
 *		A generic implementation is provided at cpuid_read_generic().
 */
struct cpuid_scan_entry {
	unsigned int	leaf;
	unsigned int	subleaf;
	unsigned int	leaf_offs;
	unsigned int	info_offs;
	unsigned int	maxcnt;
	void		(*read)(const struct cpuid_scan_entry *e, struct cpuid_read_output *o);
};

/**
 * SCAN_ENTRY() - Define a struct cpuid_scan_entry entry for @_leaf/@_subleaf
 * @_leaf:	Leaf number in 0xN format
 * @_subleaf:	Subleaf number in decimal
 * @_reader:	Read function suffix, to CPUID query @_leaf/@_subleaf
 */
#define SCAN_ENTRY(_leaf, _subleaf, _reader)					\
	{									\
		.leaf		= _leaf,					\
		.subleaf	= _subleaf,					\
		.leaf_offs	= __cpuid_leaves_leaf_offset(_leaf, _subleaf),	\
		.info_offs	= __cpuid_leaves_info_offset(_leaf, _subleaf),	\
		.maxcnt		= __cpuid_leaves_leaf_maxcnt(_leaf, _subleaf),	\
		.read		= cpuid_read_ ## _reader,			\
	}

#define CPUID_SCAN_ENTRIES							\
	/*	   leaf		subleaf		reader */			\
	SCAN_ENTRY(0x0,		0,		generic),			\
	SCAN_ENTRY(0x1,		0,		generic),			\
	SCAN_ENTRY(0x80000000,  0,              0x80000000),

/**
 * struct cpuid_scan_info - Parameters for generic CPUID scan logic
 * @cpuid_table:	CPUID table for saving scan output
 * @entries:		Leaf/subleaf scan entries
 * @nr_entries:		Array size of @entries
 */
struct cpuid_scan_info {
	struct cpuid_table		*cpuid_table;
	const struct cpuid_scan_entry	*entries;
	unsigned int			nr_entries;
};

#endif /* _ARCH_X86_CPUID_SCANNER_H */
