/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_TYPES_H
#define _ASM_X86_CPUID_TYPES_H

#include <linux/build_bug.h>
#include <linux/compiler_attributes.h>
#include <linux/types.h>

#include <asm/cpuid.h>

/*
 * CPUID(0x2) parsing helpers
 * Check for_each_leaf_0x2_entry() documentation.
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

/**
 * get_leaf_0x2_regs() - Return sanitized leaf 0x2 register output
 * @regs:	Output parameter
 *
 * Get leaf 0x2 register output and store it in @regs.  Invalid byte
 * descriptors returned by the hardware will be force set to zero (the
 * NULL cache/TLB descriptor) before returning them to the caller.
 */
static inline void get_leaf_0x2_regs(union leaf_0x2_regs *regs)
{
	cpuid_leaf(0x2, regs);

	/*
	 * All Intel CPUs must report an iteration count of 1.  In case
	 * of bogus hardware, treat all returned descriptors as NULL.
	 */
	if (regs->desc[0] != 0x01) {
		for (int i = 0; i < 4; i++)
			regs->regv[i] = 0;
		return;
	}

	/*
	 * The most significant bit (MSB) of each register must be clear.
	 * If a register is invalid, replace its descriptors with NULL.
	 */
	for (int i = 0; i < 4; i++) {
		if (regs->reg[i].invalid)
			regs->regv[i] = 0;
	}
}

/**
 * for_each_leaf_0x2_desc() - Iterator for leaf 0x2 descriptors
 * @regs:	Leaf 0x2 register output, as returned by get_leaf_0x2_regs()
 * @desc:	Pointer to the returned descriptor for each iteration
 *
 * Loop over the 1-byte descriptors in the passed leaf 0x2 output registers
 * @regs.  Provide each descriptor through @desc.
 *
 * Sample usage::
 *
 *	union leaf_0x2_regs regs;
 *	u8 *desc;
 *
 *	get_leaf_0x2_regs(&regs);
 *	for_each_leaf_0x2_desc(regs, desc) {
 *		// Handle *desc value
 *	}
 */
#define for_each_leaf_0x2_desc(regs, desc)				\
	/* Skip the first byte as it is not a descriptor */		\
	for (desc = &(regs).desc[1]; desc < &(regs).desc[16]; desc++)

/*
 * Leaf 0x2 1-byte descriptors' cache types
 * To be used for their mappings at cpuid_0x2_table[].
 *
 * Start at 1 since type 0 is reserved for HW byte descriptors which are
 * not recognized by the kernel; i.e., those without an explicit mapping
 * entry at cpuid_0x2_table[].
 */
enum _cache_table_type {
	CACHE_L1_INST	= 1,
	CACHE_L1_DATA,
	CACHE_L2,
	CACHE_L3
	/* Adjust __TLB_TABLE_TYPE_BEGIN before adding more types */
} __packed;
static_assert(sizeof(enum _cache_table_type) == 1);

/*
 * Ensure that leaf 0x2 cache and TLB type values do not intersect,
 * since they share the same field at struct cpuid_0x2_table.
 */
#define __TLB_TABLE_TYPE_BEGIN		(CACHE_L3 + 1)

/*
 * Leaf 0x2 1-byte descriptors' TLB types
 * To be used for their mappings at intel_tlb_table[]
 */
enum _tlb_table_type {
	TLB_INST_4K	= __TLB_TABLE_TYPE_BEGIN,
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
static_assert(sizeof(enum _tlb_table_type) == 1);

/*
 * Combined table for leaf 0x2 cache and TLB descriptors.
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

/**
 * for_each_leaf_0x2_entry() - Iterator for parsed leaf 0x2 descriptors
 * @regs:   Leaf 0x2 register output, as returned by get_leaf_0x2_regs()
 * @__ptr:  u8 pointer, for macro internal use only
 * @entry:  Pointer to parsed descriptor information for each iteration
 *
 * Loop over the 1-byte descriptors in the passed leaf 0x2 output registers
 * @regs.  Provide the parsed information for each descriptor through @entry.
 * To handle cache specific descriptors, switch on @entry->c_type.  For TLB
 * specific descriptors, switch on @entry->t_type.
 *
 * Example usage for cache descriptors::
 *
 *	const struct leaf_0x2_table *entry;
 *	union leaf_0x2_regs regs;
 *	u8 *ptr;
 *
 *	get_leaf_0x2_regs(&regs);
 *	for_each_leaf_0x2_entry(regs, ptr, entry) {
 *		switch (entry->c_type) {
 *			...
 *		}
 *	}
 *
 */
#define for_each_leaf_0x2_entry(regs, __ptr, entry)				\
	/* Skip the first byte as it is not a descriptor */			\
	for (__ptr = &(regs).desc[1], entry = &cpuid_0x2_table[*__ptr];		\
	     __ptr < &(regs).desc[16];						\
	     __ptr++, entry = &cpuid_0x2_table[*__ptr])

#endif /* _ASM_X86_CPUID_TYPES_H */
