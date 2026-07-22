/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_RISCV_FUTEX_ROBUST_H
#define _ASM_RISCV_FUTEX_ROBUST_H

#include <asm/ptrace.h>
#include <asm/vdso/futex.h>

static inline void __user *arch_futex_robust_unlock_get_pop(struct pt_regs *regs)
{
	if (cpu_supports_zacas())
		return (regs->a0 == regs->a1) ? (void __user *)regs->a2 : NULL;
	else
		return (regs->t0 == 0) ? (void __user *)regs->a2 : NULL;
}

#endif /* _ASM_RISCV_FUTEX_ROBUST_H */
