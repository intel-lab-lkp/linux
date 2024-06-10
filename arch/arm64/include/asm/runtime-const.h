/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RUNTIME_CONST_H
#define _ASM_RUNTIME_CONST_H

#define runtime_const_ptr(sym) ({				\
	typeof(sym) __ret;					\
	asm_inline("1:\t"					\
		"movz %0, #0xcdef\n\t"				\
		"movk %0, #0x89ab, lsl #16\n\t"			\
		"movk %0, #0x4567, lsl #32\n\t"			\
		"movk %0, #0x0123, lsl #48\n\t"			\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		:"=r" (__ret));					\
	__ret; })

#define runtime_const_shift_right_32(val, sym) ({		\
	unsigned long __ret;					\
	asm_inline("1:\t"					\
		"lsr %w0,%w1,#12\n\t"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		:"=r" (__ret)					\
		:"r" (0u+(val)));				\
	__ret; })

#define runtime_const_init(type, sym) do {		\
	extern s32 __start_runtime_##type##_##sym[];	\
	extern s32 __stop_runtime_##type##_##sym[];	\
	runtime_const_fixup(__runtime_fixup_##type,	\
		(unsigned long)(sym), 			\
		__start_runtime_##type##_##sym,		\
		__stop_runtime_##type##_##sym);		\
} while (0)

// 16-bit immediate for wide move (movz and movk) in bits 5..20
static inline void __runtime_fixup_16(unsigned int *p, unsigned int val)
{
	unsigned int insn = *p;
	insn &= 0xffe0001f;
	insn |= (val & 0xffff) << 5;
	*p = insn;
}

static inline void __runtime_fixup_ptr(void *where, unsigned long val)
{
	unsigned int *p = lm_alias(where);
	__runtime_fixup_16(p, val);
	__runtime_fixup_16(p+1, val >> 16);
	__runtime_fixup_16(p+2, val >> 32);
	__runtime_fixup_16(p+3, val >> 48);
}

// Immediate value is 5 bits starting at bit #16
static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	unsigned int *p = lm_alias(where);
	unsigned int insn = *p;
	insn &= 0xffc0ffff;
	insn |= (val & 63) << 16;
	*p = insn;
}

static inline void runtime_const_fixup(void (*fn)(void *, unsigned long),
	unsigned long val, s32 *start, s32 *end)
{
	while (start < end) {
		fn(*start + (void *)start, val);
		start++;
	}
}

#endif
