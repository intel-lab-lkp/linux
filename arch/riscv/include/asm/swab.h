/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_SWAB_H
#define _ASM_RISCV_SWAB_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/alternative-macros.h>
#include <asm/hwcap.h>

#if defined(CONFIG_RISCV_ISA_ZBB) && !defined(NO_ALTERNATIVE)

/*
 * FIXME, RFC PATCH: This is copypasted from include/uapi/linux/swab.h
 * should I move these `#defines` to include/uapi/asm-generic/swab.h
 * and include that file here and in include/uapi/linux/swab.h ?
 */
#define ___constant_swab16(x) ((__u16)(				\
	(((__u16)(x) & (__u16)0x00ffU) << 8) |			\
	(((__u16)(x) & (__u16)0xff00U) >> 8)))

#define ___constant_swab32(x) ((__u32)(				\
	(((__u32)(x) & (__u32)0x000000ffUL) << 24) |		\
	(((__u32)(x) & (__u32)0x0000ff00UL) <<  8) |		\
	(((__u32)(x) & (__u32)0x00ff0000UL) >>  8) |		\
	(((__u32)(x) & (__u32)0xff000000UL) >> 24)))

#define ___constant_swab64(x) ((__u64)(				\
	(((__u64)(x) & (__u64)0x00000000000000ffULL) << 56) |	\
	(((__u64)(x) & (__u64)0x000000000000ff00ULL) << 40) |	\
	(((__u64)(x) & (__u64)0x0000000000ff0000ULL) << 24) |	\
	(((__u64)(x) & (__u64)0x00000000ff000000ULL) <<  8) |	\
	(((__u64)(x) & (__u64)0x000000ff00000000ULL) >>  8) |	\
	(((__u64)(x) & (__u64)0x0000ff0000000000ULL) >> 24) |	\
	(((__u64)(x) & (__u64)0x00ff000000000000ULL) >> 40) |	\
	(((__u64)(x) & (__u64)0xff00000000000000ULL) >> 56)))

#define ___constant_swahw32(x) ((__u32)(			\
	(((__u32)(x) & (__u32)0x0000ffffUL) << 16) |		\
	(((__u32)(x) & (__u32)0xffff0000UL) >> 16)))

#define ___constant_swahb32(x) ((__u32)(			\
	(((__u32)(x) & (__u32)0x00ff00ffUL) << 8) |		\
	(((__u32)(x) & (__u32)0xff00ff00UL) >> 8)))


#define ARCH_SWAB(size) \
static __always_inline unsigned long __arch_swab##size(__u##size value) \
{									\
	unsigned long x = value;					\
									\
	asm goto(ALTERNATIVE("j %l[legacy]", "nop", 0,			\
			     RISCV_ISA_EXT_ZBB, 1)			\
			     :::: legacy);				\
									\
	asm volatile (".option push\n"					\
		      ".option arch,+zbb\n"				\
		      "rev8 %0, %1\n"					\
		      ".option pop\n"					\
		      : "=r" (x) : "r" (x));				\
									\
	return x >> (BITS_PER_LONG - size);				\
									\
legacy:									\
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
