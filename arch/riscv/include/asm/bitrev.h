/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_BITREV_H
#define __ASM_BITREV_H

#include <linux/types.h>
#include <asm/cpufeature-macros.h>
#include <asm/hwcap.h>

extern u8 const byte_rev_table[256];
static __always_inline __attribute_const__ u32 __arch_bitrev32(u32 x)
{
	if (IS_ENABLED(CONFIG_RISCV_ISA_ZBKB) &&
	    riscv_has_extension_likely(RISCV_ISA_EXT_ZBKB)) {
		unsigned long result = x;

		asm volatile(
			".option push\n"
			".option arch,+zbkb\n"
			"rev8 %0, %0\n"
			"brev8 %0, %0\n"
			".option pop"
			: "+r" (result)
		);

		if (__riscv_xlen == 64)
			return (u32)(result >> 32);

		return (u32)result;
	}

	return (u32)byte_rev_table[x & 0xff] << 24 |
	       (u32)byte_rev_table[(x >> 8) & 0xff] << 16 |
	       (u32)byte_rev_table[(x >> 16) & 0xff] << 8 |
	       (u32)byte_rev_table[x >> 24];
}

static __always_inline __attribute_const__ u16 __arch_bitrev16(u16 x)
{
	return __arch_bitrev32((u32)x) >> 16;
}

static __always_inline __attribute_const__ u8 __arch_bitrev8(u8 x)
{
	return __arch_bitrev32((u32)x) >> 24;
}
#endif
