#ifndef _ASM_UML_UNWIND_H
#define _ASM_UML_UNWIND_H

#ifdef CONFIG_UNWINDER_ORC

#include <linux/sched.h>
#include <linux/ftrace.h>
#include <asm/ptrace.h>
#include <asm/stacktrace.h>

struct unwind_state {
	struct stack_info stack_info;
	unsigned long stack_mask;
	struct task_struct *task;
	int graph_idx;
	bool error;
	bool signal;
	unsigned long sp, bp, ip;
	struct pt_regs *regs;
};

void __unwind_start(struct unwind_state *state, struct task_struct *task,
		    struct pt_regs *regs, unsigned long *first_frame);
bool unwind_next_frame(struct unwind_state *state);
unsigned long unwind_get_return_address(struct unwind_state *state);

static inline bool unwind_done(struct unwind_state *state)
{
	return state->stack_info.type == STACK_TYPE_UNKNOWN;
}

static inline bool unwind_error(struct unwind_state *state)
{
	return state->error;
}

static inline
void unwind_start(struct unwind_state *state, struct task_struct *task,
		  struct pt_regs *regs, unsigned long *first_frame)
{
	first_frame = first_frame ? : get_stack_pointer(task, regs);

	__unwind_start(state, task, regs, first_frame);
}

void unwind_init(void);

/* No HAVE_RETHOOK on UML, so only ftrace_graph needs to be undone. */
static inline
unsigned long unwind_recover_ret_addr(struct unwind_state *state,
				      unsigned long addr, unsigned long *addr_p)
{
	return ftrace_graph_ret_addr(state->task, &state->graph_idx,
				     addr, addr_p);
}

static inline bool task_on_another_cpu(struct task_struct *task)
{
#ifdef CONFIG_SMP
	return task != current && task->on_cpu;
#else
	return false;
#endif
}

#else /* !CONFIG_UNWINDER_ORC */

static inline void unwind_init(void) {}

#endif /* CONFIG_UNWINDER_ORC */

#endif /* _ASM_UML_UNWIND_H */
