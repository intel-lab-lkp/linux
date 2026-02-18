/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_PARAVIRT_MSR_H
#define _ASM_X86_PARAVIRT_MSR_H

#include <asm/paravirt_types.h>

struct pv_msr_ops {
	/* Unsafe MSR operations.  These will warn or panic on failure. */
	u64 (*read_msr)(u32 msr);
	void (*write_msr)(u32 msr, u64 val);

	/* Safe MSR operations.  Returns 0 or -EIO. */
	int (*read_msr_safe)(u32 msr, u64 *val);
	int (*write_msr_safe)(u32 msr, u64 val);

	u64 (*read_pmc)(int counter);
} __no_randomize_layout;

extern struct pv_msr_ops pv_ops_msr;

static __always_inline u64 read_msr(u32 msr)
{
	return PVOP_CALL1(u64, pv_ops_msr, read_msr, msr);
}

static __always_inline void write_msr(u32 msr, u64 val)
{
	PVOP_VCALL2(pv_ops_msr, write_msr, msr, val);
}

static __always_inline int read_msr_safe(u32 msr, u64 *val)
{
	return PVOP_CALL2(int, pv_ops_msr, read_msr_safe, msr, val);
}

static __always_inline int write_msr_safe(u32 msr, u64 val)
{
	return PVOP_CALL2(int, pv_ops_msr, write_msr_safe, msr, val);
}

static __always_inline u64 rdpmc(int counter)
{
	return PVOP_CALL1(u64, pv_ops_msr, read_pmc, counter);
}

#endif /* _ASM_X86_PARAVIRT_MSR_H */
