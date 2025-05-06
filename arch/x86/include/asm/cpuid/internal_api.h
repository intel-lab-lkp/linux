/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_INTERNAL_API_H
#define _ASM_X86_CPUID_INTERNAL_API_H

/*
 * Raw 'struct cpuid_leaves' accessors
 */

#include <asm/cpuid/types.h>

/**
 * __cpuid_get() - Get scanned CPUID output (without sanity checks)
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	Leaf number in the 0xN format
 * @_subleaf:	Subleaf number in decimal
 * @_idx:	@_leaf/@_subleaf query output storage index
 *
 * Return the scanned CPUID output in a ready to parse <cpuid/leaves.h> type:
 * struct leaf_0xN_M, where 0xN is the token provided at @_leaf, and M is the
 * token provided at @_subleaf.
 */
#define __cpuid_get(__leaves, __leaf, __subleaf, __idx)			\
	((__leaves)->leaf_ ## __leaf ## _ ## __subleaf)[__idx]

/**
 * __cpuid_info_get() - Get @_leaf/@_subleaf CPUID query info
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	Leaf number in the 0xN format
 * @_subleaf:	Subleaf number in decimal
 *
 * Return @_leaves repository scanned @_leaf/@_subleaf CPUID query info, as
 * &struct leaf_query_info.
 */
#define __cpuid_info_get(__leaves, __leaf, __subleaf)			\
	((__leaves)->leaf_ ## __leaf ## _ ## __subleaf ## _ ## info)

/**
 * cpuid_get() - Get scanned CPUID output (without sanity checks)
 * @_leaves:	&struct cpuid_leaves instance
 * @_leaf:	Leaf number in the 0xN format
 *
 * Like __cpuid_get(), but with the subleaf and output storage index assumed
 * as zero.
 */
#define cpuid_get(_leaves, _leaf)					\
	__cpuid_get(_leaves, _leaf, 0, 0)

/*
 * struct cpuid_table accessors (with sanity checks)
 *
 * Return requested data as a <cpuid/leaves.h> data type, or NULL if the
 * entry is not available.
 */

#define __cpudata_cpuid_subleaf_idx(__table, __leaf, __subleaf, __idx)	\
	((__cpuid_info_get(&((__table)->leaves), __leaf, __subleaf).nr_entries > __idx) ? \
	 &__cpuid_get(&((__table)->leaves), __leaf, __subleaf, __idx) : NULL)

#define __cpudata_cpuid_subleaf(__table, __leaf, __subleaf)		\
	__cpudata_cpuid_subleaf_idx(__table, __leaf, __subleaf, 0)

#endif /* _ASM_X86_CPUID_INTERNAL_API_H */
