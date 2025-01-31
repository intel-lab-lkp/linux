/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_RUNTIME_CONST_H
#define _ASM_RISCV_RUNTIME_CONST_H

#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/text-patching.h>
#include <linux/uaccess.h>

#ifdef CONFIG_32BIT
#define runtime_const_ptr(sym)					\
({								\
	typeof(sym) __ret, __tmp;				\
	asm_inline(".align 4\n\t"				\
		".option push\n\t"				\
		".option norvc\n\t"				\
		"1:\t"						\
		"lui	%[__ret],0x89abd\n\t"			\
		"addi	%[__ret],-0x211\n\t"			\
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
#define runtime_const_ptr(sym)						\
({									\
	typeof(sym) __ret, __tmp;					\
	asm_inline(".align 4\n\t"					\
		".option push\n\t"					\
		".option norvc\n\t"					\
		"1:\t"							\
		"lui	%[__ret],0x89abd\n\t"				\
		"lui	%[__tmp],0x1234\n\t"				\
		"addiw	%[__ret],%[__ret],-0x211\n\t"			\
		"addiw	%[__tmp],%[__tmp],0x567\n\t"			\
		ALTERNATIVE_2(						\
			"slli	%[__tmp],%[__tmp],32\n\t"		\
			"slli	%[__ret],%[__ret],32\n\t"		\
			"srli	%[__ret],%[__ret],32\n\t"		\
			"add	%[__ret],%[__ret],%[__tmp]\n\t",	\
			".option push\n\t"				\
			".option arch,+zba\n\t"				\
			"slli	%[__tmp],%[__tmp],32\n\t"		\
			"add.uw %[__ret],%[__ret],%[__tmp]\n\t"		\
			"nop\n\t"					\
			"nop\n\t"					\
			".option pop\n\t",				\
			0, RISCV_ISA_EXT_ZBA, 1,			\
			".option push\n\t"				\
			".option arch,+zbkb\n\t"			\
			"pack	%[__ret],%[__ret],%[__tmp]\n\t"		\
			"nop\n\t"					\
			"nop\n\t"					\
			"nop\n\t"					\
			".option pop\n\t",				\
			0, RISCV_ISA_EXT_ZBKB, 1			\
		)							\
		".option pop\n\t"					\
		".pushsection runtime_ptr_" #sym ",\"a\"\n\t"		\
		".long 1b - .\n\t"					\
		".popsection"						\
		: [__ret] "=r" (__ret), [__tmp] "=r" (__tmp));		\
	__ret;								\
})
#endif

#ifdef CONFIG_32BIT
#define SRLI "srli "
#else
#define SRLI "srliw "
#endif

#define runtime_const_shift_right_32(val, sym)			\
({								\
	u32 __ret;						\
	asm_inline(".align 4\n\t"				\
		".option push\n\t"				\
		".option norvc\n\t"				\
		"1:\t"						\
		SRLI "%[__ret],%[__val],12\n\t"			\
		".option pop\n\t"				\
		".pushsection runtime_shift_" #sym ",\"a\"\n\t"	\
		".long 1b - .\n\t"				\
		".popsection"					\
		: [__ret] "=r" (__ret)				\
		: [__val] "r" (val));				\
	__ret;							\
})

#define runtime_const_init(type, sym) do {			\
	extern s32 __start_runtime_##type##_##sym[];		\
	extern s32 __stop_runtime_##type##_##sym[];		\
								\
	runtime_const_fixup(__runtime_fixup_##type,		\
			    (unsigned long)(sym),		\
			    __start_runtime_##type##_##sym,	\
			    __stop_runtime_##type##_##sym);	\
} while (0)

static inline void __runtime_fixup_caches(void *where, unsigned int insns)
{
	/* On riscv there are currently only cache-wide flushes so va is ignored. */
	__always_unused uintptr_t va = (uintptr_t)where;

	flush_icache_range(va, va + 4*insns);
}

/*
 * The 32-bit immediate is stored in a lui+addi pairing.
 * lui holds the upper 20 bits of the immediate in the first 20 bits of the instruction.
 * addi holds the lower 12 bits of the immediate in the first 12 bits of the instruction.
 */
static inline void __runtime_fixup_32(u32 *lui, u32 *addi, unsigned int val)
{
	unsigned int lower_immediate, upper_immediate;
	u32 lui_insn = le32_to_cpu(*lui);
	u32 addi_insn = le32_to_cpu(*addi);
	__le32 addi_res, lui_res;

	lower_immediate = sign_extend32(val, 11);
	upper_immediate = (val - lower_immediate);

	if (upper_immediate & 0xfffff000) {
		/* replace upper 20 bits of lui with upper immediate */
		lui_insn &= 0x00000fff;
		lui_insn |= upper_immediate & 0xfffff000;
	} else {
		/* replace lui with nop if immediate is small enough to fit in addi */
		lui_insn = 0x00000013;
	}

	if (lower_immediate & 0x00000fff) {
		/* replace upper 12 bits of addi with lower 12 bits of val */
		addi_insn &= 0x000fffff;
		addi_insn |= (lower_immediate & 0x00000fff) << 20;
	} else {
		/* replace addi with nop if lower_immediate is empty */
		addi_insn = 0x00000013;
	}

	addi_res = cpu_to_le32(addi_insn);
	lui_res = cpu_to_le32(lui_insn);
	patch_insn_write(addi, &addi_res, sizeof(addi_res));
	patch_insn_write(lui, &lui_res, sizeof(lui_res));
}

static inline void __runtime_fixup_ptr(void *where, unsigned long val)
{
	if (IS_ENABLED(CONFIG_32BIT)) {
		__runtime_fixup_32(where, where + 4, val);
		__runtime_fixup_caches(where, 2);
	} else {
		__runtime_fixup_32(where, where + 8, val);
		__runtime_fixup_32(where + 4, where + 12, val >> 32);
		__runtime_fixup_caches(where, 4);
	}
}

/*
 * Replace the least significant 5 bits of the srli/srliw immediate that is
 * located at bits 20-24
 */
static inline void __runtime_fixup_shift(void *where, unsigned long val)
{
	u32 insn = le32_to_cpu(*(__le32 *)where);
	__le32 res;

	insn &= 0xfe0fffff;
	insn |= (val & 0b11111) << 20;

	res = cpu_to_le32(insn);
	patch_text_nosync(where, &res, sizeof(insn));
}

static inline void runtime_const_fixup(void (*fn)(void *, unsigned long),
				       unsigned long val, s32 *start, s32 *end)
{
	while (start < end) {
		fn(*start + (void *)start, val);
		start++;
	}
}

#endif /* _ASM_RISCV_RUNTIME_CONST_H */
