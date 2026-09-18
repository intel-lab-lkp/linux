/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_FUTEX_ROBUST_H
#define _ASM_ARM64_FUTEX_ROBUST_H

#include <asm/ptrace.h>

static __always_inline void __user *arm64_futex_robust_unlock_get_pop(struct pt_regs *regs)
{
	/*
	 * In the asm for __vdso_futex_robust_list{64,32}_try_unlock(), w3
	 * stores the result of the stlxr instruction. If it's zero, the then
	 * the ll/sc cmpxchg succeeded and the pending op pointer needs to be
	 * cleared.
	 */
	if (regs->regs[3])
		return NULL;

	return (void __user *)regs->regs[2];
}

#define arch_futex_robust_unlock_get_pop(regs) \
	arm64_futex_robust_unlock_get_pop(regs)

#endif /* _ASM_ARM64_FUTEX_ROBUST_H */
