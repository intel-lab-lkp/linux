// SPDX-License-Identifier: GPL-2.0
#include <asm/entry.h>
#include <linux/context_tracking.h>
#include <linux/entry-common.h>
#include <linux/irqflags.h>
#include <linux/rseq.h>

static irqentry_state_t user_irq_state;
static irqentry_state_t kernel_irq_state;
static irqentry_state_t user_nmi_state;
static irqentry_state_t kernel_nmi_state;

noinstr void arm_irqentry_enter_from_user_mode(struct pt_regs *regs)
{
	user_irq_state = irqentry_enter(regs);
}

noinstr void arm_irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	irqentry_exit(regs, user_irq_state);
}

noinstr void arm_irqentry_enter_from_kernel_mode(struct pt_regs *regs)
{
	kernel_irq_state = irqentry_enter(regs);
}

noinstr void arm_irqentry_exit_to_kernel_mode(struct pt_regs *regs)
{
	irqentry_exit(regs, kernel_irq_state);
}

noinstr void arm_irqentry_nmi_enter_from_user_mode(struct pt_regs *regs)
{
	irqentry_enter_from_user_mode(regs);
	user_nmi_state = irqentry_nmi_enter(regs);
}

noinstr void arm_irqentry_nmi_exit_to_user_mode(struct pt_regs *regs)
{
	irqentry_nmi_exit(regs, user_nmi_state);
	irqentry_exit_to_user_mode(regs);
}

noinstr void arm_irqentry_nmi_enter_from_kernel_mode(struct pt_regs *regs)
{
	kernel_nmi_state = irqentry_nmi_enter(regs);
}

noinstr void arm_irqentry_nmi_exit_to_kernel_mode(struct pt_regs *regs)
{
	irqentry_nmi_exit(regs, kernel_nmi_state);
}

asmlinkage void arm_exit_to_user_mode(struct pt_regs *regs)
{
	local_irq_disable();
	irqentry_exit_to_user_mode(regs);
}
