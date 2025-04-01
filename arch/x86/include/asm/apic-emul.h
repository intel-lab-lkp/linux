/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_APIC_EMUL_H
#define _ASM_X86_APIC_EMUL_H

#define MAX_APIC_VECTOR			256
#define APIC_VECTORS_PER_REG		32

static inline int apic_find_highest_vector(void *bitmap)
{
	unsigned int regno;
	unsigned int vec;
	u32 *reg;

	/*
	 * The registers int the bitmap are 32-bit wide and 16-byte
	 * aligned. State of a vector is stored in a single bit.
	 */
	for (regno = MAX_APIC_VECTOR / APIC_VECTORS_PER_REG - 1; regno >= 0; regno--) {
		vec = regno * APIC_VECTORS_PER_REG;
		reg = bitmap + regno * 16;
		if (*reg)
			return __fls(*reg) + vec;
	}

	return -1;
}

#endif /* _ASM_X86_APIC_EMUL_H */
