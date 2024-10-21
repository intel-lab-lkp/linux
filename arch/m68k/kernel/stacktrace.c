// SPDX-License-Identifier: GPL-2.0

/*
 * Stack trace utility functions etc.
 *
 * Copyright 2024 Jean-Michel Hautbois, Yoseli SAS.
 */

#include <asm/current.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>

static inline unsigned long current_stack_frame(void)
{
	unsigned long sp;

	asm volatile("movl %%sp, %0" : "=r"(sp));
	return sp;
}

static inline int validate_sp(unsigned long sp, struct task_struct *task)
{
	unsigned long stack_start, stack_end;

	if (task == current)
		stack_start = (unsigned long)task_stack_page(task);
	else
		stack_start = (unsigned long)task->thread.esp0;

	stack_end = stack_start + THREAD_SIZE;

	if (sp < stack_start || sp >= stack_end)
		return 0;

	return 1;
}

void __no_sanitize_address arch_stack_walk(stack_trace_consume_fn consume_entry, void *cookie,
					   struct task_struct *task, struct pt_regs *regs)
{
	unsigned long sp;

	if (regs && !consume_entry(cookie, regs->pc))
		return;

	if (regs)
		sp = (unsigned long) regs;
	else if (task == current)
		sp = current_stack_frame();
	else
		sp = task->thread.ksp;

	for (;;) {
		unsigned long *stack = (unsigned long *) sp;
		unsigned long newsp, ip;

		if (!validate_sp(sp, task))
			return;

		newsp = stack[0];
		ip = stack[1];

		if (!consume_entry(cookie, ip))
			return;

		sp = newsp;
	}
}
