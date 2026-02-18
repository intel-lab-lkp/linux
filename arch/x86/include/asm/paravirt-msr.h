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

static __always_inline u64 read_msr(u32 msr)
{
	u64 val;

	asm volatile(PARAVIRT_CALL
		     : "=a" (val), ASM_CALL_CONSTRAINT
		     : paravirt_ptr(pv_ops_msr, read_msr), "c" (msr)
		     : "rdx");

	return val;
}

static __always_inline void write_msr(u32 msr, u64 val)
{
	asm volatile(PARAVIRT_CALL
		     : ASM_CALL_CONSTRAINT
		     : paravirt_ptr(pv_ops_msr, write_msr), "c" (msr), "a" (val)
		     : "memory", "rdx");
}

static __always_inline int read_msr_safe(u32 msr, u64 *val)
{
	int err;

	asm volatile(PARAVIRT_CALL
		     : [err] "=d" (err), "=a" (*val), ASM_CALL_CONSTRAINT
		     : paravirt_ptr(pv_ops_msr, read_msr_safe), "c" (msr));

	return err ? -EIO : 0;
}

static __always_inline int write_msr_safe(u32 msr, u64 val)
{
	int err;

	asm volatile(PARAVIRT_CALL
		     : [err] "=a" (err), ASM_CALL_CONSTRAINT
		     : paravirt_ptr(pv_ops_msr, write_msr_safe),
			"c" (msr), "a" (val)
		     : "memory", "rdx");

	return err ? -EIO : 0;
}

static __always_inline u64 rdpmc(int counter)
{
	return PVOP_CALL1(u64, pv_ops_msr, read_pmc, counter);
}

#endif /* _ASM_X86_PARAVIRT_MSR_H */
