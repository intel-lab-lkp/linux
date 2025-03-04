/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPUID_TYPES_H
#define _ASM_X86_CPUID_TYPES_H

#include <linux/types.h>

#include <asm/cpuid.h>

/*
 * CPUID(0x2) parsing helpers
 * Check for_each_leaf_0x2_desc() documentation.
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

#endif /* _ASM_X86_CPUID_TYPES_H */
