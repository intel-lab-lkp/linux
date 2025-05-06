/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_LEAF_0x2_API_H
#define _ASM_X86_CPUID_LEAF_0x2_API_H

#include <asm/cpuid/types.h>

/**
 * for_each_leaf_0x2_entry() - Iterator for parsed leaf 0x2 descriptors
 * @regs:   Leaf 0x2 register output, as returned by cpudata_cpuid_regs()
 * @__ptr:  u8 pointer, for macro internal use only
 * @entry:  Pointer to parsed descriptor information at each iteration
 *
 * Loop over the 1-byte descriptors in the passed CPUID(0x2) output registers
 * @regs.  Provide the parsed information for each descriptor through @entry.
 *
 * To handle cache-specific descriptors, switch on @entry->c_type.  For TLB
 * descriptors, switch on @entry->t_type.
 *
 * Example usage for cache descriptors::
 *
 *	const struct leaf_0x2_table *entry;
 *	struct cpuid_regs *regs;
 *	u8 *ptr;
 *
 *	regs = cpudata_cpuid_regs(c, 0x2);
 *	if (!regs) {
 *		// Handle error
 *	}
 *
 *	for_each_scanned_leaf_0x2_entry(regs, ptr, entry) {
 *		switch (entry->c_type) {
 *			...
 *		}
 *	}
 */
#define for_each_leaf_0x2_entry(regs, __ptr, entry)					\
	for (({ static_assert(sizeof(*regs) == sizeof(union leaf_0x2_regs)); }),	\
	     __ptr = &((union leaf_0x2_regs *)(regs))->desc[1];				\
	     __ptr < &((union leaf_0x2_regs *)(regs))->desc[16] && (entry = &cpuid_0x2_table[*__ptr]);\
	     __ptr++)

#endif /* _ASM_X86_CPUID_LEAF_0x2_API_H */
