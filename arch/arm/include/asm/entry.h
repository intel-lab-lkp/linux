/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ENTRY_H__
#define __ASM_ENTRY_H__

struct pt_regs;

/*
 * These are copies of generic entry headers so we can transition
 * to generic entry once they are semantically equivalent.
 */
long syscall_enter_from_user_mode(struct pt_regs *regs, long);
void syscall_exit_to_user_mode(struct pt_regs *regs);
void irqentry_enter_from_user_mode(struct pt_regs *regs);
void irqentry_exit_to_user_mode(struct pt_regs *regs);
void irqentry_enter_from_kernel_mode(struct pt_regs *regs);
void irqentry_exit_to_kernel_mode(struct pt_regs *regs);

#endif /* __ASM_ENTRY_H__ */
