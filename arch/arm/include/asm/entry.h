/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ENTRY_H__
#define __ASM_ENTRY_H__

struct pt_regs;

void arm_irqentry_enter_from_user_mode(struct pt_regs *regs);
void arm_irqentry_exit_to_user_mode(struct pt_regs *regs);
void arm_irqentry_enter_from_kernel_mode(struct pt_regs *regs);
void arm_irqentry_exit_to_kernel_mode(struct pt_regs *regs);
void arm_irqentry_nmi_enter_from_user_mode(struct pt_regs *regs);
void arm_irqentry_nmi_exit_to_user_mode(struct pt_regs *regs);
void arm_irqentry_nmi_enter_from_kernel_mode(struct pt_regs *regs);
void arm_irqentry_nmi_exit_to_kernel_mode(struct pt_regs *regs);
void arm_exit_to_user_mode(struct pt_regs *regs);

#endif /* __ASM_ENTRY_H__ */
