/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UML_STACKTRACE_H
#define _ASM_UML_STACKTRACE_H

#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/sched/task_stack.h>

struct stack_frame {
	struct stack_frame *next_frame;
	unsigned long return_address;
};

struct stacktrace_ops {
	void (*address)(void *data, unsigned long address, int reliable);
};

#ifdef CONFIG_FRAME_POINTER
static inline unsigned long
get_frame_pointer(struct task_struct *task, struct pt_regs *segv_regs)
{
	if (!task || task == current)
		return segv_regs ? PT_REGS_BP(segv_regs) : current_bp();
	return KSTK_EBP(task);
}
#else
static inline unsigned long
get_frame_pointer(struct task_struct *task, struct pt_regs *segv_regs)
{
	return 0;
}
#endif

static inline unsigned long
*get_stack_pointer(struct task_struct *task, struct pt_regs *segv_regs)
{
	if (!task || task == current)
		return segv_regs ? (unsigned long *)PT_REGS_SP(segv_regs) : current_sp();
	return (unsigned long *)KSTK_ESP(task);
}

void dump_trace(struct task_struct *tsk, const struct stacktrace_ops *ops, void *data);

#ifdef CONFIG_UNWINDER_ORC

#include <linux/smp-internal.h>

/* host signal handlers run on the per-CPU cpu_irqstacks (sigaltstack) */
enum stack_type {
	STACK_TYPE_UNKNOWN,
	STACK_TYPE_TASK,
	STACK_TYPE_IRQ,
};

struct stack_info {
	enum stack_type type;
	unsigned long *begin, *end, *next_sp;
};

static inline bool on_stack(struct stack_info *info, void *addr, size_t len)
{
	void *begin = info->begin;
	void *end = info->end;

	return info->type != STACK_TYPE_UNKNOWN &&
	       addr >= begin && addr < end &&
	       addr + len > begin && addr + len <= end;
}

static inline bool in_task_stack(unsigned long *stack, struct task_struct *task,
				 struct stack_info *info)
{
	unsigned long *begin = task_stack_page(task);
	unsigned long *end = task_stack_page(task) + THREAD_SIZE;

	if (stack < begin || stack >= end)
		return false;

	info->type = STACK_TYPE_TASK;
	info->begin = begin;
	info->end = end;
	info->next_sp = NULL;

	return true;
}

static inline bool in_irq_stack(unsigned long *stack, struct stack_info *info)
{
	void *begin = cpu_irqstacks;
	void *end = cpu_irqstacks + NR_CPUS;

	if ((void *)stack < begin || (void *)stack >= end)
		return false;

	begin = PTR_ALIGN_DOWN(stack, THREAD_SIZE);
	info->type = STACK_TYPE_IRQ;
	info->begin = begin;
	info->end = begin + THREAD_SIZE;
	info->next_sp = NULL;

	return true;
}

static inline int get_stack_info(unsigned long *stack, struct task_struct *task,
				 struct stack_info *info, unsigned long *visit_mask)
{
	task = task ? : current;

	if (!stack)
		goto unknown;

	if (!in_task_stack(stack, task, info) &&
	    (task != current || !in_irq_stack(stack, info)))
		goto unknown;

	if (visit_mask) {
		if (*visit_mask & (1UL << info->type))
			goto unknown;
		*visit_mask |= 1UL << info->type;
	}

	return 0;

unknown:
	info->type = STACK_TYPE_UNKNOWN;
	return -EINVAL;
}
#endif /* CONFIG_UNWINDER_ORC */

#endif /* _ASM_UML_STACKTRACE_H */
