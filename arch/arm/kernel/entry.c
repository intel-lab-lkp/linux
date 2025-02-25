// SPDX-License-Identifier: GPL-2.0
#include <asm/entry.h>
#include <linux/context_tracking.h>
#include <linux/irqflags.h>

long syscall_enter_from_user_mode(struct pt_regs *regs, long syscall)
{
	trace_hardirqs_on();
	local_irq_enable();
	/* This context tracking call has inverse naming */
	user_exit_callable();

	/* This will optionally be modified later */
	return syscall;
}

noinstr void irqentry_enter_from_user_mode(struct pt_regs *regs)
{
	trace_hardirqs_off();
	/* This context tracking call has inverse naming */
	user_exit_callable();
}

noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	trace_hardirqs_on();
	/* This context tracking call has inverse naming */
	user_enter_callable();
}
