// SPDX-License-Identifier: GPL-2.0
#include <asm/entry.h>
#include <linux/context_tracking.h>
#include <linux/entry-common.h>
#include <linux/hardirq.h>
#include <linux/irq.h>
#include <linux/irqflags.h>
#include <linux/percpu.h>
#include <linux/rseq.h>
#include <asm/stacktrace.h>

#include "irq.h"

static void noinstr handle_arm_irq(void *data)
{
	struct pt_regs *regs = data;
	struct pt_regs *old_regs;

	irq_enter_rcu();
	old_regs = set_irq_regs(regs);

	handle_arch_irq(regs);

	set_irq_regs(old_regs);
	irq_exit_rcu();
}

noinstr void arm_irq_handler(struct pt_regs *regs, int mode)
{
	irqentry_state_t state = irqentry_enter(regs);

	/*
	 * If we are executing in usermode, or kernel process context
	 * (on the thread stack) then switch to the IRQ stack. Else we
	 * are already on the IRQ stack (or the overflow stack) and we
	 * can just proceed to handle the IRQ.
	 */
	if (mode == 1)
		call_on_irq_stack(handle_arm_irq, regs);
	else if (on_thread_stack())
		call_on_irq_stack(handle_arm_irq, regs);
	else
		handle_arm_irq(regs);

	irqentry_exit(regs, state);
}

/*
 * Handle FIQ similarly to NMI on x86 systems.
 *
 * The runtime environment for NMIs is extremely restrictive
 * (NMIs can pre-empt critical sections meaning almost all locking is
 * forbidden) meaning this default FIQ handling must only be used in
 * circumstances where non-maskability improves robustness, such as
 * watchdog or debug logic.
 *
 * This handler is not appropriate for general purpose use in drivers
 * platform code and can be overrideen using set_fiq_handler.
 */
noinstr void arm_fiq_handler(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_nmi_enter(regs);

	irqentry_nmi_exit(regs, state);
}

asmlinkage void arm_exit_to_user_mode(struct pt_regs *regs)
{
	local_irq_disable();
	irqentry_exit_to_user_mode(regs);
}
