/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ENTRY_H__
#define __ASM_ENTRY_H__

struct pt_regs;

void arm_und_handler(struct pt_regs *regs);
void arm_dabt_handler(unsigned long addr, unsigned int fsr, struct pt_regs *regs);
void arm_pabt_handler(unsigned long addr, unsigned int ifsr, struct pt_regs *regs);
void arm_irq_handler(struct pt_regs *regs, int mode);
void arm_fiq_handler(struct pt_regs *regs);
void arm_exit_to_user_mode(struct pt_regs *regs);

#endif /* __ASM_ENTRY_H__ */
