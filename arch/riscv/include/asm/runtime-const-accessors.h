/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_RUNTIME_CONST_ACCESSORS_H
#define _ASM_RISCV_RUNTIME_CONST_ACCESSORS_H

#ifdef MODULE
#error "this functionality is not available for modules"
#endif

#ifdef CONFIG_32BIT
#define runtime_const_ptr(sym)					\
({								\
	typeof(sym) __ret;					\
	asm_inline(".option push\n\t"				\
		".option norvc\n\t"				\
		"1:\t"						\
		"lui	%[__ret],0x89abd\n\t"			\
		"addi	%[__ret],%[__ret],-0x211\n\t"		\
		".option pop\n\t"				\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: [__ret] "=r" (__ret));			\
	__ret;							\
})
#else
/*
 * Loading 64-bit constants into a register from immediates is a non-trivial
 * task on riscv64. To get it somewhat performant, load 32 bits into two
 * different registers and then combine the results.
 *
 * If the processor supports the Zbkb extension, we can combine the final
 * "slli,slli,srli,add" into the single "pack" instruction. If the processor
 * doesn't support Zbkb but does support the Zbb extension, we can
 * combine the final "slli,srli,add" into one instruction "add.uw".
 */
#define RISCV_RUNTIME_CONST_64_PREAMBLE				\
	".option push\n\t"					\
	".option norvc\n\t"					\
	"1:\t"							\
	"lui	%[__ret],0x89abd\n\t"				\
	"lui	%[__tmp],0x1234\n\t"				\
	"addiw	%[__ret],%[__ret],-0x211\n\t"			\
	"addiw	%[__tmp],%[__tmp],0x567\n\t"			\

#define RISCV_RUNTIME_CONST_64_BASE				\
	"slli	%[__tmp],%[__tmp],32\n\t"			\
	"slli	%[__ret],%[__ret],32\n\t"			\
	"srli	%[__ret],%[__ret],32\n\t"			\
	"add	%[__ret],%[__ret],%[__tmp]\n\t"			\

#define RISCV_RUNTIME_CONST_64_ZBA				\
	".option push\n\t"					\
	".option arch,+zba\n\t"					\
	".option norvc\n\t"					\
	"slli	%[__tmp],%[__tmp],32\n\t"			\
	"add.uw %[__ret],%[__ret],%[__tmp]\n\t"			\
	"nop\n\t"						\
	"nop\n\t"						\
	".option pop\n\t"					\

#define RISCV_RUNTIME_CONST_64_ZBKB				\
	".option push\n\t"					\
	".option arch,+zbkb\n\t"				\
	".option norvc\n\t"					\
	"pack	%[__ret],%[__ret],%[__tmp]\n\t"			\
	"nop\n\t"						\
	"nop\n\t"						\
	"nop\n\t"						\
	".option pop\n\t"					\

#define RISCV_RUNTIME_CONST_64_POSTAMBLE(sym)			\
	".option pop\n\t"					\
	".pushsection runtime_ptr_" #sym ",\"a\"\n\t"		\
	".long 1b - .\n\t"					\
	".popsection"						\

#if defined(CONFIG_RISCV_ISA_ZBA) && defined(CONFIG_TOOLCHAIN_HAS_ZBA)	\
	&& defined(CONFIG_RISCV_ISA_ZBKB)
#define runtime_const_ptr(sym)						\
({									\
	typeof(sym) __ret, __tmp;					\
	asm_inline(RISCV_RUNTIME_CONST_64_PREAMBLE			\
		ALTERNATIVE_2(						\
			RISCV_RUNTIME_CONST_64_BASE,			\
			RISCV_RUNTIME_CONST_64_ZBA,			\
			0, RISCV_ISA_EXT_ZBA, 1,			\
			RISCV_RUNTIME_CONST_64_ZBKB,			\
			0, RISCV_ISA_EXT_ZBKB, 1			\
		)							\
		RISCV_RUNTIME_CONST_64_POSTAMBLE(sym)			\
		: [__ret] "=r" (__ret), [__tmp] "=r" (__tmp));		\
	__ret;								\
})
#elif defined(CONFIG_RISCV_ISA_ZBA) && defined(CONFIG_TOOLCHAIN_HAS_ZBA)
#define runtime_const_ptr(sym)						\
({									\
	typeof(sym) __ret, __tmp;					\
	asm_inline(RISCV_RUNTIME_CONST_64_PREAMBLE			\
		ALTERNATIVE(						\
			RISCV_RUNTIME_CONST_64_BASE,			\
			RISCV_RUNTIME_CONST_64_ZBA,			\
			0, RISCV_ISA_EXT_ZBA, 1				\
		)							\
		RISCV_RUNTIME_CONST_64_POSTAMBLE(sym)			\
		: [__ret] "=r" (__ret), [__tmp] "=r" (__tmp));		\
	__ret;								\
})
#elif defined(CONFIG_RISCV_ISA_ZBKB)
#define runtime_const_ptr(sym)						\
({									\
	typeof(sym) __ret, __tmp;					\
	asm_inline(RISCV_RUNTIME_CONST_64_PREAMBLE			\
		ALTERNATIVE(						\
			RISCV_RUNTIME_CONST_64_BASE,			\
			RISCV_RUNTIME_CONST_64_ZBKB,			\
			0, RISCV_ISA_EXT_ZBKB, 1			\
		)							\
		RISCV_RUNTIME_CONST_64_POSTAMBLE(sym)			\
		: [__ret] "=r" (__ret), [__tmp] "=r" (__tmp));		\
	__ret;								\
})
#else
#define runtime_const_ptr(sym)						\
({									\
	typeof(sym) __ret, __tmp;					\
	asm_inline(RISCV_RUNTIME_CONST_64_PREAMBLE			\
		RISCV_RUNTIME_CONST_64_BASE				\
		RISCV_RUNTIME_CONST_64_POSTAMBLE(sym)			\
		: [__ret] "=r" (__ret), [__tmp] "=r" (__tmp));		\
	__ret;								\
})
#endif
#endif

#define runtime_const_shift_right_32(val, sym)			\
({								\
	u32 __ret;						\
	asm_inline(".option push\n\t"				\
		".option norvc\n\t"				\
		"1:\t"						\
		SRLI " %[__ret],%[__val],12\n\t"		\
		".option pop\n\t"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: [__ret] "=r" (__ret)				\
		: [__val] "r" (val));				\
	__ret;							\
})

#endif /* _ASM_RISCV_RUNTIME_CONST_ACCESSORS_H */
