// SPDX-License-Identifier: GPL-2.0
#include <asm/entry.h>
#include <asm/ptrace.h>
#include <asm/signal.h>
#include <linux/context_tracking.h>
#include <linux/irqflags.h>
#include <linux/rseq.h>

long syscall_enter_from_user_mode(struct pt_regs *regs, long syscall)
{
	trace_hardirqs_on();
	local_irq_enable();
	/* This context tracking call has inverse naming */
	user_exit_callable();

	/* This will optionally be modified later */
	return syscall;
}

void syscall_exit_to_user_mode(struct pt_regs *regs)
{
	unsigned long flags = read_thread_flags();

	rseq_syscall(regs);
	local_irq_disable();
	/*
	 * It really matters that we check for flags != 0 and not
	 * just for pending work here!
	 */
	if (flags)
		do_work_pending(regs, flags);

	trace_hardirqs_on();
	/* This context tracking call has inverse naming */
	user_enter_callable();
}

noinstr void irqentry_enter_from_user_mode(struct pt_regs *regs)
{
	trace_hardirqs_off();
	/* This context tracking call has inverse naming */
	user_exit_callable();
}

noinstr void irqentry_exit_to_user_mode(struct pt_regs *regs)
{
	unsigned long flags = read_thread_flags();

	/*
	 * It really matters that we check for flags != 0 and not
	 * just for pending work here!
	 */
	if (flags)
		do_work_pending(regs, flags);
	trace_hardirqs_on();
	/* This context tracking call has inverse naming */
	user_enter_callable();
}

noinstr void irqentry_enter_from_kernel_mode(struct pt_regs *regs)
{
}

noinstr void irqentry_exit_to_kernel_mode(struct pt_regs *regs)
{
}
