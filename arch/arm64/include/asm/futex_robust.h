/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_FUTEX_ROBUST_H
#define _ASM_ARM64_FUTEX_ROBUST_H

#include <asm/ptrace.h>

static __always_inline void __user *arm64_futex_robust_unlock_get_pop(struct pt_regs *regs)
{
	/*
	 * w3 is stores the result of the stlxr instruction. If it's zero, the then
	 * the ll/sc cmpxchg succeeded and the pending op pointer needs to be cleared.
	 */
	return (regs->user_regs.regs[3]) ? NULL : (void __user *) regs->user_regs.regs[2];
}

#define arch_futex_robust_unlock_get_pop(regs)	\
	arm64_futex_robust_unlock_get_pop(regs)

#endif /* _ASM_ARM64_FUTEX_ROBUST_H */
