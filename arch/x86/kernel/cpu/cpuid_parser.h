/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_X86_CPUID_PARSER_H
#define _ARCH_X86_CPUID_PARSER_H

#include <linux/types.h>
#include <asm/cpuid/types.h>

/*
 * 'struct cpuid_leaves' CPUID query output storage area accessors:
 *
 * @_leaf:	CPUID leaf, in compile-time 0xN format
 * @_subleaf:	CPUID subleaf, in compile-time decimal format
 *
 * Since accessing the CPUID leaf output storage areas at 'struct cpuid_leaves' requires
 * compile time tokenization, split the CPUID parser implementation into two stages:
 * compile time macros for tokenizing the leaf/subleaf output offsets within the CPUID
 * table, and generic runtime code to access and populate the relevant CPUID leaf/subleaf
 * output data structures using such offsets.
 *
 * That is, the output of the  __cpuid_leaves_query_*_offset() macros will be cached by a
 * compile time "parse entry" (see 'struct cpuid_parse_entry').  The runtime parser code
 * will then utilize such offsets by passing them to cpuid_table_query_*_p() functions.
 */

#define __cpuid_leaves_query_regs_offset(_leaf, _subleaf)			\
	offsetof(struct cpuid_leaves, leaf_ ## _leaf ## _ ## _subleaf)

#define __cpuid_leaves_query_info_offset(_leaf, _subleaf)			\
	offsetof(struct cpuid_leaves, leaf_ ## _leaf ## _ ## _subleaf ## _ ## info)

#define __cpuid_leaves_query_regs_maxcnt(_leaf, _subleaf)			\
	ARRAY_SIZE(((struct cpuid_leaves *)NULL)->leaf_ ## _leaf ## _ ## _subleaf)

static inline struct cpuid_regs *
cpuid_table_query_regs_p(const struct cpuid_table *t, unsigned long regs_offset)
{
	return (struct cpuid_regs *)((unsigned long)(&t->leaves) + regs_offset);
}

static inline struct leaf_query_info *
cpuid_table_query_info_p(const struct cpuid_table *t, unsigned long info_offset)
{
	return (struct leaf_query_info *)((unsigned long)(&t->leaves) + info_offset);
}

/**
 * struct cpuid_read_output - Output of a CPUID parser read operation
 * @regs:	Pointer to an array of CPUID outputs, where each array element covers the
 *		full EAX->EDX output range.
 * @info:	Pointer to query info; for saving the number of filled @regs array elements.
 *
 * A CPUID parser read function like cpuid_read_generic() or cpuid_read_0xN() uses this
 * structure to save its CPUID query outputs.  Actual storage for @regs and @info is provided
 * by its caller, and is typically within a CPU's CPUID table (struct cpuid_table.leaves).
 *
 * See struct cpuid_parse_entry.read().
 */
struct cpuid_read_output {
	struct cpuid_regs	*regs;
	struct leaf_query_info	*info;
};

/**
 * struct cpuid_parse_entry - Runtime CPUID parsing context for @leaf/@subleaf
 * @leaf:	Leaf number to be parsed
 * @subleaf:	Subleaf number to be parsed
 * @regs_offs:	Offset within 'struct cpuid_leaves' for saving CPUID @leaf/@subleaf output; to be
 *		passed to cpuid_table_query_regs_p().
 * @info_offs:	Offset within 'struct cpuid_leaves' for accessing @leaf/@subleaf parse info; to be
 *		passed to cpuid_table_query_info_p().
 * @maxcnt:	Maximum number of output storage entries available for the @leaf/@subleaf query
 * @read:	Read function for this entry.  It must save the parsed CPUID output to the passed
 *		'struct cpuid_read_output'->regs registers array of size >= @maxcnt.  It must set
 *		'struct cpuid_read_output'->info.nr_entries to the actual number of storage output
 *		entries filled.  A generic implementation is provided at cpuid_read_generic().
 */
struct cpuid_parse_entry {
	unsigned int	leaf;
	unsigned int	subleaf;
	unsigned int	regs_offs;
	unsigned int	info_offs;
	unsigned int	maxcnt;
	void		(*read)(const struct cpuid_parse_entry *e, struct cpuid_read_output *o);
};

#define __CPUID_PARSE_ENTRY(_leaf, _subleaf, _suffix, _reader_fn)			\
	{										\
		.leaf		= _leaf,						\
		.subleaf	= _subleaf,						\
		.regs_offs	= __cpuid_leaves_query_regs_offset(_leaf, _suffix),	\
		.info_offs	= __cpuid_leaves_query_info_offset(_leaf, _suffix),	\
		.maxcnt		= __cpuid_leaves_query_regs_maxcnt(_leaf, _suffix),	\
		.read		= cpuid_read_ ## _reader_fn,				\
	}

/*
 * CPUID_PARSE_ENTRY_N() is for CPUID leaves with a dynamic subleaf range.
 * Check <asm/cpuid/types.h> __CPUID_LEAF() and CPUID_LEAF_N().
 */

#define CPUID_PARSE_ENTRY(_leaf, _subleaf, _reader_fn)					\
	__CPUID_PARSE_ENTRY(_leaf, _subleaf, _subleaf, _reader_fn)

#define CPUID_PARSE_ENTRY_N(_leaf, _reader_fn)						\
	__CPUID_PARSE_ENTRY(_leaf, __cpuid_leaf_first_dynamic_subleaf(_leaf), n, _reader_fn)

/*
 * CPUID parser tables:
 */

/*
 * Early-boot CPUID leaves (to be parsed before x86 vendor detection)
 *
 * These leaves must be parsed at early boot to identify the x86 vendor. The
 * parser treats them as universally valid across all vendors.
 *
 * At early boot, only leaves in this table must be parsed.  For all other
 * leaves, the CPUID parser will assume that "boot_cpu_data.x86_vendor" is
 * properly set beforehand.
 *
 * Note: If these entries are to be modified, please adapt the kernel-doc of
 * cpuid_parser_early_scan_cpu() accordingly.
 */
#define CPUID_EARLY_PARSE_ENTRIES								\
	/*			Leaf		Subleaf		Reader function */		\
	CPUID_PARSE_ENTRY   (	0x0,		0,		generic			),	\
	CPUID_PARSE_ENTRY   (	0x1,		0,		generic			),	\

/*
 * Common CPUID leaves
 *
 * These leaves can be parsed once basic x86 vendor detection is in place.
 * Further vendor-agnostic leaves, which are not needed at early boot, are also
 * listed here.
 *
 * For vendor-specific leaves, a matching entry must be added to the CPUID leaf
 * vendor table later defined.  Leaves which are here, but without a matching
 * vendor entry, are treated by the CPUID parser as valid for all x86 vendors.
 */
#define CPUID_COMMON_PARSE_ENTRIES								\
	/*			Leaf		Static subleaf	Reader function */		\
	CPUID_PARSE_ENTRY   (	0x2,		0,		0x2			),	\
	CPUID_PARSE_ENTRY_N (	0x4,				deterministic_cache	),	\
	CPUID_PARSE_ENTRY   (	0x80000000,	0,		0x80000000		),	\
	CPUID_PARSE_ENTRY   (	0x80000002,	0,		generic			),	\
	CPUID_PARSE_ENTRY   (	0x80000003,	0,		generic			),	\
	CPUID_PARSE_ENTRY   (	0x80000004,	0,		generic			),	\
	CPUID_PARSE_ENTRY   (	0x80000005,	0,		generic			),	\
	CPUID_PARSE_ENTRY   (	0x80000006,	0,		generic			),	\
	CPUID_PARSE_ENTRY_N (	0x8000001d,			deterministic_cache	),	\

/*
 * CPUID parser tables repository:
 */

struct cpuid_parser_phase {
	const struct cpuid_parse_entry	*table;
	int				nr_entries;
};

extern const struct cpuid_parser_phase cpuid_parser_phases[];
extern const int cpuid_parser_nphases;

/*
 * CPUID leaf vendor table:
 */

struct cpuid_vendor_entry {
	unsigned int	leaf;
	u8		vendors[X86_VENDOR_NUM];
	u8		nvendors;
};

#define CPUID_VENDOR_ENTRY(_leaf, ...)							\
	{										\
		.leaf		= _leaf,						\
		.vendors	= { __VA_ARGS__ },					\
		.nvendors	= (sizeof((u8[]){__VA_ARGS__})/sizeof(u8)),		\
	}

#define CPUID_VENDOR_ENTRIES								\
	/*		   Leaf		Vendor list		    */			\
	CPUID_VENDOR_ENTRY(0x2,		X86_VENDOR_INTEL, X86_VENDOR_CENTAUR, X86_VENDOR_ZHAOXIN),\
	CPUID_VENDOR_ENTRY(0x4,		X86_VENDOR_INTEL, X86_VENDOR_CENTAUR, X86_VENDOR_ZHAOXIN),\
	CPUID_VENDOR_ENTRY(0x8000001d,	X86_VENDOR_AMD, X86_VENDOR_HYGON),		\

#endif /* _ARCH_X86_CPUID_PARSER_H */
