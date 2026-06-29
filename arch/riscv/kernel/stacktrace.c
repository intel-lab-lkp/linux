// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2008 ARM Limited
 * Copyright (C) 2014 Regents of the University of California
 */

#include <linux/export.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/llist.h>

#include <asm/stacktrace.h>

/*
 * Non-frame-pointer fallback unwinder.
 * Only compiled when CONFIG_FRAME_POINTER is not enabled.
 */
#ifndef CONFIG_FRAME_POINTER

void notrace walk_stackframe(struct task_struct *task,
	struct pt_regs *regs, bool (*fn)(void *, unsigned long), void *arg)
{
	unsigned long sp, pc;
	unsigned long *ksp;

	if (regs) {
		sp = user_stack_pointer(regs);
		pc = instruction_pointer(regs);
	} else if (task == NULL || task == current) {
		sp = current_stack_pointer;
		pc = (unsigned long)walk_stackframe;
	} else {
		/* task blocked in __switch_to */
		sp = task->thread.sp;
		pc = task->thread.ra;
	}

	if (unlikely(sp & 0x7))
		return;

	ksp = (unsigned long *)sp;
	while (!kstack_end(ksp)) {
		if (__kernel_text_address(pc) && unlikely(!fn(arg, pc)))
			break;
		pc = READ_ONCE_NOCHECK(*ksp++);
	}
}

#endif /* !CONFIG_FRAME_POINTER */

/*
 * Common trace helpers.
 * These are used by both the FP (kunwind) and non-FP (walk_stackframe) paths.
 */

static bool print_trace_address(void *arg, unsigned long pc)
{
	const char *loglvl = arg;

	print_ip_sym(loglvl, pc);
	return true;
}

noinline void dump_backtrace(struct pt_regs *regs, struct task_struct *task,
		    const char *loglvl)
{
	printk("%sCall Trace:\n", loglvl);
	arch_stack_walk(print_trace_address, (void *)loglvl, task, regs);
}

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	dump_backtrace(NULL, task, loglvl);
}

static bool save_wchan(void *arg, unsigned long pc)
{
	if (!in_sched_functions(pc)) {
		unsigned long *p = arg;
		*p = pc;
		return false;
	}
	return true;
}

unsigned long __get_wchan(struct task_struct *task)
{
	unsigned long pc = 0;

	if (!try_get_task_stack(task))
		return 0;
	arch_stack_walk(save_wchan, &pc, task, NULL);
	put_task_stack(task);
	return pc;
}

/*
 * Frame-pointer-based kernel unwind infrastructure.
 * Only compiled when CONFIG_FRAME_POINTER is enabled.
 *
 * See: arch/arm64/kernel/stacktrace.c for the reference implementation.
 */
#ifdef CONFIG_FRAME_POINTER

/*
 * Per-cpu stacks are only accessible when unwinding the current task in a
 * non-preemptible context.
 */
#define STACKINFO_CPU(task, name)				\
	({							\
		(((task) == current) && !preemptible())		\
			? stackinfo_get_##name()		\
			: stackinfo_get_unknown();		\
	})

enum kunwind_source {
	KUNWIND_SOURCE_UNKNOWN,
	KUNWIND_SOURCE_FRAME,
	KUNWIND_SOURCE_CALLER,
	KUNWIND_SOURCE_TASK,
	KUNWIND_SOURCE_REGS_PC,
};

union unwind_flags {
	unsigned long	all;
	struct {
		unsigned long	fgraph : 1,
				kretprobe : 1;
	};
};

/*
 * Kernel unwind state
 *
 * @common:    Common unwind state.
 * @task:      The task being unwound.
 * @graph_idx: Used by ftrace_graph_ret_addr() for optimized stack unwinding.
 * @kr_cur:    When KRETPROBES is selected, holds the kretprobe instance
 *             associated with the most recently encountered replacement ra
 *             value.
 */
struct kunwind_state {
	struct unwind_state common;
	struct task_struct *task;
	int graph_idx;
#ifdef CONFIG_KRETPROBES
	struct llist_node *kr_cur;
#endif
	enum kunwind_source source;
	union unwind_flags flags;
	struct pt_regs *regs;
};

static __always_inline void
kunwind_init(struct kunwind_state *state,
	     struct task_struct *task)
{
	unwind_init_common(&state->common);
	state->task = task;
	state->source = KUNWIND_SOURCE_UNKNOWN;
	state->flags.all = 0;
	state->regs = NULL;
}

/*
 * Start an unwind from a pt_regs.
 *
 * The unwind will begin at the PC within the regs.
 *
 * The regs must be on a stack currently owned by the calling task.
 */
static __always_inline void
kunwind_init_from_regs(struct kunwind_state *state,
		       struct pt_regs *regs)
{
	kunwind_init(state, current);

	state->regs = regs;
	state->common.fp = frame_pointer(regs);
	state->common.pc = instruction_pointer(regs);
	state->source = KUNWIND_SOURCE_REGS_PC;
}

/*
 * Start an unwind from a caller.
 *
 * The unwind will begin at the caller of whichever function this is inlined
 * into.
 *
 * The function which invokes this must be noinline.
 */
static __always_inline void
kunwind_init_from_caller(struct kunwind_state *state)
{
	unsigned long fp = (unsigned long)__builtin_frame_address(0);
	struct frame_record *record = (struct frame_record *)fp - 1;

	kunwind_init(state, current);

	state->common.fp = READ_ONCE(record->fp);
	state->common.pc = READ_ONCE(record->ra);
	state->source = KUNWIND_SOURCE_CALLER;
}

/*
 * Start an unwind from a blocked task.
 *
 * The unwind will begin at the blocked task's saved PC (i.e. the caller of
 * __switch_to).
 *
 * The caller should ensure the task is blocked in __switch_to for the
 * duration of the unwind, or the unwind will be bogus. It is never valid to
 * call this for the current task.
 */
static __always_inline void
kunwind_init_from_task(struct kunwind_state *state,
		       struct task_struct *task)
{
	kunwind_init(state, task);

	state->common.fp = task->thread.s[0];
	state->common.pc = task->thread.ra;
	state->source = KUNWIND_SOURCE_TASK;
}

static __always_inline int
kunwind_recover_return_address(struct kunwind_state *state)
{
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	if (state->task->ret_stack &&
	    state->common.pc == (unsigned long)return_to_handler) {
		unsigned long orig_pc;

		orig_pc = ftrace_graph_ret_addr(state->task, &state->graph_idx,
						state->common.pc,
						(void *)state->common.fp);
		if (state->common.pc == orig_pc) {
			WARN_ON_ONCE(state->task == current);
			return -EINVAL;
		}
		state->common.pc = orig_pc;
		state->flags.fgraph = 1;
	}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

#ifdef CONFIG_KRETPROBES
	if (is_kretprobe_trampoline(state->common.pc)) {
		unsigned long orig_pc;

		orig_pc = kretprobe_find_ret_addr(state->task,
						  (void *)state->common.fp,
						  &state->kr_cur);
		if (!orig_pc)
			return -EINVAL;
		state->common.pc = orig_pc;
		state->flags.kretprobe = 1;
	}
#endif /* CONFIG_KRETPROBES */

	return 0;
}

/*
 * When we reach an exception boundary marked by a metadata frame record,
 * extract pt_regs from the stack and continue unwinding from the saved
 * context (epc and s0/fp).
 *
 * On RISC-V, fp points above the metadata record, so the record's
 * frame_record portion is at fp - sizeof(struct frame_record).
 */
static __always_inline int
kunwind_next_regs_pc(struct kunwind_state *state)
{
	struct stack_info *info;
	unsigned long fp = state->common.fp;
	struct pt_regs *regs;

	regs = container_of((unsigned long *)(fp - sizeof(struct frame_record)),
			    struct pt_regs, stackframe.record.fp);

	info = unwind_find_stack(&state->common, (unsigned long)regs,
				 sizeof(*regs));
	if (!info)
		return -EINVAL;

	unwind_consume_stack(&state->common, info, (unsigned long)regs,
			     sizeof(*regs));

	state->regs = regs;
	state->common.pc = regs->epc;
	state->common.fp = frame_pointer(regs);
	state->source = KUNWIND_SOURCE_REGS_PC;
	return 0;
}

/*
 * Handle a metadata frame record embedded in pt_regs.
 *
 * On RISC-V, fp points above the record (fp = metadata + 16), so the
 * frame_record_meta starts at fp - sizeof(struct frame_record).
 *
 * FRAME_META_TYPE_FINAL: This is the outermost exception entry
 *   (user -> kernel). Unwinding terminates successfully.
 * FRAME_META_TYPE_PT_REGS: This is a nested exception entry
 *   (kernel -> kernel). Continue unwinding from the saved context.
 */
static __always_inline int
kunwind_next_frame_record_meta(struct kunwind_state *state)
{
	struct task_struct *tsk = state->task;
	unsigned long fp = state->common.fp;
	unsigned long meta_base = fp - sizeof(struct frame_record);
	struct frame_record_meta *meta;
	struct stack_info *info;

	info = unwind_find_stack(&state->common, meta_base, sizeof(*meta));
	if (!info)
		return -EINVAL;

	meta = (struct frame_record_meta *)meta_base;
	switch (READ_ONCE(meta->type)) {
	case FRAME_META_TYPE_FINAL:
		if (meta == &task_pt_regs(tsk)->stackframe)
			return -ENOENT;
		WARN_ON_ONCE(tsk == current);
		return -EINVAL;
	case FRAME_META_TYPE_PT_REGS:
		return kunwind_next_regs_pc(state);
	default:
		WARN_ON_ONCE(tsk == current);
		return -EINVAL;
	}
}

/*
 * Unwind from one frame record to the next.
 *
 * On RISC-V, the frame record sits at fp - sizeof(struct frame_record),
 * immediately below the address pointed to by fp/s0. This applies to both
 * normal frame records and metadata frame records (embedded in pt_regs).
 *
 * A metadata record is identified by both fp and ra being zero in the
 * frame_record portion, with a type value following at fp + 16.
 */
static __always_inline int
kunwind_next_frame_record(struct kunwind_state *state)
{
	unsigned long fp = state->common.fp;
	struct frame_record *record;
	struct stack_info *info;
	unsigned long new_fp, new_pc;
	unsigned long record_base;

	if (fp & 0x7)
		return -EINVAL;

	record_base = fp - sizeof(*record);

	info = unwind_find_stack(&state->common, record_base, sizeof(*record));
	if (!info)
		return -EINVAL;

	record = (struct frame_record *)record_base;
	new_fp = READ_ONCE(record->fp);
	new_pc = READ_ONCE(record->ra);

	if (!new_fp && !new_pc)
		return kunwind_next_frame_record_meta(state);

	unwind_consume_stack(&state->common, info, record_base,
			     sizeof(*record));

	state->common.fp = new_fp;
	state->common.pc = new_pc;
	state->source = KUNWIND_SOURCE_FRAME;

	return 0;
}

/*
 * Unwind from one frame record (A) to the next frame record (B).
 *
 * We terminate early if the location of B indicates a malformed chain of frame
 * records (e.g. a cycle), determined based on the location and fp value of A
 * and the location (but not the fp value) of B.
 */
static __always_inline int
kunwind_next(struct kunwind_state *state)
{
	int err;

	state->flags.all = 0;

	switch (state->source) {
	case KUNWIND_SOURCE_FRAME:
	case KUNWIND_SOURCE_CALLER:
	case KUNWIND_SOURCE_TASK:
	case KUNWIND_SOURCE_REGS_PC:
		err = kunwind_next_frame_record(state);
		break;
	default:
		err = -EINVAL;
	}

	if (err)
		return err;

	return kunwind_recover_return_address(state);
}

typedef bool (*kunwind_consume_fn)(const struct kunwind_state *state, void *cookie);

static __always_inline int
do_kunwind(struct kunwind_state *state, kunwind_consume_fn consume_state,
	   void *cookie)
{
	int ret;

	ret = kunwind_recover_return_address(state);
	if (ret)
		return ret;

	while (1) {
		if (!consume_state(state, cookie))
			return -EINVAL;
		ret = kunwind_next(state);
		if (ret == -ENOENT)
			return 0;
		if (ret < 0)
			return ret;
	}
}

static __always_inline int
kunwind_stack_walk(kunwind_consume_fn consume_state,
		   void *cookie, struct task_struct *task,
		   struct pt_regs *regs)
{
	struct task_struct *tsk = task ?: current;
	struct stack_info stacks[] = {
		stackinfo_get_task(tsk),
		STACKINFO_CPU(tsk, irq),
#ifdef CONFIG_VMAP_STACK
		STACKINFO_CPU(tsk, overflow),
#endif
	};
	struct kunwind_state state = {
		.common = {
			.stacks = stacks,
			.nr_stacks = ARRAY_SIZE(stacks),
		},
	};

	if (regs) {
		if (tsk != current)
			return -EINVAL;
		kunwind_init_from_regs(&state, regs);
	} else if (tsk == current) {
		kunwind_init_from_caller(&state);
	} else {
		kunwind_init_from_task(&state, tsk);
	}

	return do_kunwind(&state, consume_state, cookie);
}

struct kunwind_consume_entry_data {
	stack_trace_consume_fn consume_entry;
	void *cookie;
};

static __always_inline bool
arch_kunwind_consume_entry(const struct kunwind_state *state, void *cookie)
{
	struct kunwind_consume_entry_data *data = cookie;

	return data->consume_entry(data->cookie, state->common.pc);
}

static __always_inline bool
arch_reliable_kunwind_consume_entry(const struct kunwind_state *state, void *cookie)
{
	/*
	 * At an exception boundary we can reliably consume the saved PC. We do
	 * not know whether ra was live when the exception was taken, and
	 * so we cannot perform the next unwind step reliably.
	 *
	 * All that matters is whether the *entire* unwind is reliable, so give
	 * up as soon as we hit an exception boundary.
	 */
	if (state->source == KUNWIND_SOURCE_REGS_PC)
		return false;

	return arch_kunwind_consume_entry(state, cookie);
}

#endif /* CONFIG_FRAME_POINTER */

/*
 * arch_stack_walk - dual implementation.
 *
 * When CONFIG_FRAME_POINTER is enabled, uses the kunwind infrastructure for
 * robust frame-pointer-based unwinding, consistent with arch_stack_walk_reliable.
 *
 * When CONFIG_FRAME_POINTER is disabled, falls back to the simple stack scan
 * in walk_stackframe().
 */
#ifdef CONFIG_FRAME_POINTER

noinline noinstr void arch_stack_walk(stack_trace_consume_fn consume_entry,
				      void *cookie, struct task_struct *task,
				      struct pt_regs *regs)
{
	struct kunwind_consume_entry_data data = {
		.consume_entry = consume_entry,
		.cookie = cookie,
	};

	kunwind_stack_walk(arch_kunwind_consume_entry, &data, task, regs);
}

#else

noinline noinstr void arch_stack_walk(stack_trace_consume_fn consume_entry,
				      void *cookie, struct task_struct *task,
				      struct pt_regs *regs)
{
	walk_stackframe(task, regs, consume_entry, cookie);
}

#endif /* CONFIG_FRAME_POINTER */

/*
 * Reliable stack walk for livepatch (CONFIG_FRAME_POINTER only).
 */
#ifdef CONFIG_FRAME_POINTER

noinline noinstr int arch_stack_walk_reliable(stack_trace_consume_fn consume_entry,
					      void *cookie,
					      struct task_struct *task)
{
	struct kunwind_consume_entry_data data = {
		.consume_entry = consume_entry,
		.cookie = cookie,
	};

	return kunwind_stack_walk(arch_reliable_kunwind_consume_entry, &data,
				  task, NULL);
}

#endif /* CONFIG_FRAME_POINTER */

/*
 * Get the return address for a single stackframe and return a pointer to the
 * next frame tail.
 */
static unsigned long unwind_user_frame(stack_trace_consume_fn consume_entry,
				       void *cookie, unsigned long fp,
				       unsigned long reg_ra)
{
	struct stackframe buftail;
	unsigned long ra = 0;
	unsigned long __user *user_frame_tail =
		(unsigned long __user *)(fp - sizeof(struct stackframe));

	/* Check accessibility of one struct frame_tail beyond */
	if (!access_ok(user_frame_tail, sizeof(buftail)))
		return 0;
	if (__copy_from_user_inatomic(&buftail, user_frame_tail,
				      sizeof(buftail)))
		return 0;

	ra = reg_ra ? : buftail.ra;

	fp = buftail.fp;
	if (!ra || !consume_entry(cookie, ra))
		return 0;

	return fp;
}

void arch_stack_walk_user(stack_trace_consume_fn consume_entry, void *cookie,
			  const struct pt_regs *regs)
{
	unsigned long fp = 0;

	fp = regs->s0;
	if (!consume_entry(cookie, regs->epc))
		return;

	fp = unwind_user_frame(consume_entry, cookie, fp, regs->ra);
	while (fp && !(fp & 0x7))
		fp = unwind_user_frame(consume_entry, cookie, fp, 0);
}
