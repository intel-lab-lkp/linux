/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_PARAVIRT_MSR_H
#define _ASM_X86_PARAVIRT_MSR_H

#include <asm/paravirt_types.h>

struct pv_msr_ops {
	/* Unsafe MSR operations.  These will warn or panic on failure. */
	struct paravirt_callee_save read_msr;
	struct paravirt_callee_save write_msr;

	/* Safe MSR operations.  Returns 0 or -EIO. */
	struct paravirt_callee_save read_msr_safe;
	struct paravirt_callee_save write_msr_safe;

	u64 (*read_pmc)(int counter);
} __no_randomize_layout;

extern struct pv_msr_ops pv_ops_msr;

#define PV_PROLOGUE_MSR(func)		\
	PV_SAVE_COMMON_CALLER_REGS	\
	PV_PROLOGUE_MSR_##func

#define PV_EPILOGUE_MSR(func)	PV_RESTORE_COMMON_CALLER_REGS

#define PV_CALLEE_SAVE_REGS_MSR_THUNK(func)		\
	__PV_CALLEE_SAVE_REGS_THUNK(func, ".text", MSR)

#define ASM_CLRERR	"xor %[err],%[err]\n"

#define PV_RDMSR_VAR(__msr, __val, __type, __func, __err)		\
	asm volatile(							\
		"1:\n"							\
		ALTERNATIVE_2(PARAVIRT_CALL,				\
			RDMSR_AND_SAVE_RESULT ASM_CLRERR, X86_FEATURE_ALWAYS, \
			ALT_CALL_INSTR, ALT_XEN_CALL)			\
		"2:\n"							\
		_ASM_EXTABLE_TYPE_REG(1b, 2b, __type, %[err])		\
		: [err] "=d" (__err), [val] "=a" (__val),		\
		  ASM_CALL_CONSTRAINT					\
		: paravirt_ptr(pv_ops_msr, __func), "c" (__msr)		\
		: "cc")

#define PV_RDMSR_CONST(__msr, __val, __type, __func, __err)		\
	asm volatile(							\
		"1:\n"							\
		ALTERNATIVE_3(PARAVIRT_CALL,				\
			RDMSR_AND_SAVE_RESULT ASM_CLRERR, X86_FEATURE_ALWAYS, \
			ASM_RDMSR_IMM ASM_CLRERR, X86_FEATURE_MSR_IMM,	\
			ALT_CALL_INSTR, ALT_XEN_CALL)			\
		"2:\n"							\
		_ASM_EXTABLE_TYPE_REG(1b, 2b, __type, %[err])		\
		: [err] "=d" (__err), [val] "=a" (__val),		\
		  ASM_CALL_CONSTRAINT					\
		: paravirt_ptr(pv_ops_msr, __func),			\
		  "c" (__msr), [msr] "i" (__msr)			\
		: "cc")

#define PV_WRMSR_VAR(__msr, __val, __type, __func, __err)		\
({									\
	unsigned long rdx = rdx;					\
	asm volatile(							\
		"1:\n"							\
		ALTERNATIVE_3(PARAVIRT_CALL,				\
			"wrmsr;" ASM_CLRERR, X86_FEATURE_ALWAYS,	\
			ASM_WRMSRNS ASM_CLRERR, X86_FEATURE_WRMSRNS,	\
			ALT_CALL_INSTR, ALT_XEN_CALL)			\
		"2:\n"							\
		_ASM_EXTABLE_TYPE_REG(1b, 2b, __type, %[err])		\
		: [err] "=a" (__err), "=d" (rdx), ASM_CALL_CONSTRAINT	\
		: paravirt_ptr(pv_ops_msr, __func),			\
		  "0" (__val), "1" ((__val) >> 32), "c" (__msr)		\
		: "memory", "cc");					\
})

#define PV_WRMSR_CONST(__msr, __val, __type, __func, __err)		\
({									\
	unsigned long rdx = rdx;					\
	asm volatile(							\
		"1:\n"							\
		ALTERNATIVE_4(PARAVIRT_CALL,				\
			"wrmsr;" ASM_CLRERR, X86_FEATURE_ALWAYS,	\
			ASM_WRMSRNS ASM_CLRERR, X86_FEATURE_WRMSRNS,	\
			ASM_WRMSRNS_IMM ASM_CLRERR, X86_FEATURE_MSR_IMM,\
			ALT_CALL_INSTR, ALT_XEN_CALL)			\
		"2:\n"							\
		_ASM_EXTABLE_TYPE_REG(1b, 2b, __type, %[err])		\
		: [err] "=a" (__err), "=d" (rdx), ASM_CALL_CONSTRAINT	\
		: paravirt_ptr(pv_ops_msr, __func),			\
		  [val] "0" (__val), "1" ((__val) >> 32),		\
		  "c" (__msr), [msr] "i" (__msr)			\
		: "memory", "cc");					\
})

static __always_inline u64 read_msr(u32 msr)
{
	u64 val;
	int err;

	if (__builtin_constant_p(msr))
		PV_RDMSR_CONST(msr, val, EX_TYPE_RDMSR, read_msr, err);
	else
		PV_RDMSR_VAR(msr, val, EX_TYPE_RDMSR, read_msr, err);

	return val;
}

static __always_inline void write_msr(u32 msr, u64 val)
{
	int err;

	if (__builtin_constant_p(msr))
		PV_WRMSR_CONST(msr, val, EX_TYPE_WRMSR, write_msr, err);
	else
		PV_WRMSR_VAR(msr, val, EX_TYPE_WRMSR, write_msr, err);
}

static __always_inline int read_msr_safe(u32 msr, u64 *val)
{
	int err;

	if (__builtin_constant_p(msr))
		PV_RDMSR_CONST(msr, *val, EX_TYPE_RDMSR_SAFE, read_msr_safe, err);
	else
		PV_RDMSR_VAR(msr, *val, EX_TYPE_RDMSR_SAFE, read_msr_safe, err);

	return err ? -EIO : 0;
}

static __always_inline int write_msr_safe(u32 msr, u64 val)
{
	int err;

	if (__builtin_constant_p(msr))
		PV_WRMSR_CONST(msr, val, EX_TYPE_WRMSR_SAFE, write_msr_safe, err);
	else
		PV_WRMSR_VAR(msr, val, EX_TYPE_WRMSR_SAFE, write_msr_safe, err);

	return err ? -EIO : 0;
}

static __always_inline u64 rdpmc(int counter)
{
	return PVOP_CALL1(u64, pv_ops_msr, read_pmc, counter);
}

#endif /* _ASM_X86_PARAVIRT_MSR_H */
