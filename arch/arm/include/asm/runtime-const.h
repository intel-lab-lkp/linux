/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM_RUNTIME_CONST_H
#define _ASM_ARM_RUNTIME_CONST_H

#ifdef MODULE
  #error "Cannot use runtime-const infrastructure from modules"
#endif

#if __LINUX_ARM_ARCH__ >= 7 && !defined(CONFIG_THUMB2_KERNEL)
#include <asm/cacheflush.h>

/* Sigh. You can still run arm in BE mode */
#include <asm/byteorder.h>

#define runtime_const_ptr(sym) ({				\
	typeof(sym) __ret;					\
	asm_inline("1:\n"					\
		"movw %0, #0x4567\n"				\
		"movt %0, #0x0123\n"				\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n"				\
		".popsection"					\
		: "=r" (__ret));				\
	__ret; })

#define runtime_const_shift_right_32(val, sym) ({		\
	unsigned long __ret;					\
	asm_inline("1:\n"					\
		"lsr %0,%1,#12\n"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n"				\
		".popsection"					\
		: "=r" (__ret)					\
		: "r" (0u+(val)));				\
	__ret; })

#define runtime_const_init(type, sym) do {			\
	extern s32 __start_runtime_##type##_##sym[];		\
	extern s32 __stop_runtime_##type##_##sym[];		\
								\
	runtime_const_fixup(__runtime_fixup_##type,		\
			    (unsigned long)(sym),		\
			    __start_runtime_##type##_##sym,	\
			    __stop_runtime_##type##_##sym);	\
} while (0)

/*
 * 16-bit immediate for wide move (movw and movt) is encoded in
 * bits 19:16, 11:0
 */
static inline void __runtime_fixup_16(__le32 *p, unsigned int val)
{
	u32 insn = le32_to_cpu(*p);

	insn &= 0xfff0f000;
	insn |= val & 0xfff;
	insn |= ((val & 0xf000) >> 12) << 16;
	*p = cpu_to_le32(insn);
}

static inline void __runtime_fixup_caches(void *where, unsigned int insns)
{
	unsigned long va = (unsigned long)where;

	flush_icache_range(va, va + 4 * insns);
}

static inline void __runtime_fixup_ptr(void *where, unsigned long val)
{
	__le32 *p = where;

	__runtime_fixup_16(p, val);
	__runtime_fixup_16(p + 1, val >> 16);
	__runtime_fixup_caches(where, 2);
}

/* Immediate value is 5 bits starting at bit #7 */
static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	__le32 *p = where;
	u32 insn = le32_to_cpu(*p);

	insn &= 0xfffff07f;
	insn |= (val & 0x1f) << 7;
	*p = cpu_to_le32(insn);
	__runtime_fixup_caches(where, 1);
}

static inline void runtime_const_fixup(void (*fn)(void *, unsigned long),
	unsigned long val, s32 *start, s32 *end)
{
	while (start < end) {
		fn(*start + (void *)start, val);
		start++;
	}
}

#else
#include <asm-generic/runtime-const.h>
#endif /* __LINUX_ARM_ARCH__ >= 7 && !defined(CONFIG_THUMB2_KERNEL) */

#endif /* _ASM_ARM_RUNTIME_CONST_H */
