/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_STACKTRACE_H
#define _ASM_RISCV_STACKTRACE_H

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>

#include <asm/irq_stack.h>
#include <asm/ptrace.h>
#include <asm/stacktrace/common.h>

struct stackframe {
	unsigned long fp;
	unsigned long ra;
};

extern void notrace walk_stackframe(struct task_struct *task, struct pt_regs *regs,
				    bool (*fn)(void *, unsigned long), void *arg);
extern void dump_backtrace(struct pt_regs *regs, struct task_struct *task,
			   const char *loglvl);

/*
 * IRQ stack accessors
 */
static inline struct stack_info stackinfo_get_irq(void)
{
	unsigned long low = (unsigned long)raw_cpu_read(irq_stack_ptr);
	unsigned long high = low + IRQ_STACK_SIZE;

	return (struct stack_info) {
		.low = low,
		.high = high,
	};
}

static inline bool on_irq_stack(unsigned long sp, unsigned long size)
{
	struct stack_info info = stackinfo_get_irq();

	return stackinfo_on_stack(&info, sp, size);
}

/*
 * Task stack accessors
 */
static inline struct stack_info stackinfo_get_task(const struct task_struct *tsk)
{
	unsigned long low = (unsigned long)task_stack_page(tsk);
	unsigned long high = low + THREAD_SIZE;

	return (struct stack_info) {
		.low = low,
		.high = high,
	};
}

static inline bool on_task_stack(const struct task_struct *tsk,
				 unsigned long sp, unsigned long size)
{
	struct stack_info info = stackinfo_get_task(tsk);

	return stackinfo_on_stack(&info, sp, size);
}

/*
 * Cast is necessary since current->stack is an opaque ptr.
 */
#define on_thread_stack()	(on_task_stack(current, current_stack_pointer, 1))

/*
 * Overflow stack accessors
 */
#ifdef CONFIG_VMAP_STACK
DECLARE_PER_CPU(unsigned long [OVERFLOW_STACK_SIZE/sizeof(long)], overflow_stack);

static inline struct stack_info stackinfo_get_overflow(void)
{
	unsigned long low = (unsigned long)raw_cpu_ptr(overflow_stack);
	unsigned long high = low + OVERFLOW_STACK_SIZE;

	return (struct stack_info) {
		.low = low,
		.high = high,
	};
}
#endif /* CONFIG_VMAP_STACK */

#endif /* _ASM_RISCV_STACKTRACE_H */
