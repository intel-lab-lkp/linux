/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_SWAB_H
#define _ASM_RISCV_SWAB_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/cpufeature-macros.h>
#include <asm/hwcap.h>
#include <asm-generic/swab.h>

#if defined(CONFIG_RISCV_ISA_ZBB) && !defined(NO_ALTERNATIVE)

#define ARCH_SWAB(size) \
static __always_inline unsigned long __arch_swab##size(__u##size value) \
{									\
	unsigned long x = value;					\
									\
	if (riscv_has_extension_likely(RISCV_ISA_EXT_ZBB)) {            \
		asm volatile (".option push\n"				\
			      ".option arch,+zbb\n"			\
			      "rev8 %0, %1\n"				\
			      ".option pop\n"				\
			      : "=r" (x) : "r" (x));			\
		return x >> (BITS_PER_LONG - size);			\
	}                                                               \
	return  ___constant_swab##size(value);				\
}

#ifdef CONFIG_64BIT
ARCH_SWAB(64)
#define __arch_swab64 __arch_swab64
#endif

ARCH_SWAB(32)
#define __arch_swab32 __arch_swab32

ARCH_SWAB(16)
#define __arch_swab16 __arch_swab16

#undef ARCH_SWAB

#endif /* defined(CONFIG_RISCV_ISA_ZBB) && !defined(NO_ALTERNATIVE) */
#endif /* _ASM_RISCV_SWAB_H */
