// SPDX-License-Identifier: GPL-2.0
/*
* Generic interfaces for unwinding user space
*/
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/unwind_user.h>
#include <linux/uaccess.h>
#include <linux/sframe.h>

static struct unwind_user_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};

static struct unwind_user_frame compat_fp_frame = {
	ARCH_INIT_USER_COMPAT_FP_FRAME
};

static inline bool fp_state(struct unwind_user_state *state)
{
	return IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP) &&
	       state->type == UNWIND_USER_TYPE_FP;
}

#define for_each_user_frame(state) \
	for (unwind_user_start(state); !(state)->done; unwind_user_next(state))

static inline bool compat_fp_state(struct unwind_user_state *state)
{
	return IS_ENABLED(CONFIG_HAVE_UNWIND_USER_COMPAT_FP) &&
	       state->type == UNWIND_USER_TYPE_COMPAT_FP;
}

static inline bool sframe_state(struct unwind_user_state *state)
{
	return IS_ENABLED(CONFIG_HAVE_UNWIND_USER_SFRAME) &&
	       state->type == UNWIND_USER_TYPE_SFRAME;
}

#define unwind_get_user_long(to, from, state)				\
({									\
	int __ret;							\
	if (compat_fp_state(state))					\
		__ret = get_user(to, (u32 __user *)(from));		\
	else								\
		__ret = get_user(to, (unsigned long __user *)(from));	\
	__ret;								\
})

static int unwind_user_next(struct unwind_user_state *state)
{
	struct unwind_user_frame *frame;
	struct unwind_user_frame _frame;
	unsigned long cfa = 0, fp, ra = 0;
	unsigned int shift;

	if (state->done)
		return -EINVAL;

	if (compat_fp_state(state)) {
		frame = &compat_fp_frame;
	} else if (sframe_state(state)) {
		/* sframe expects the frame to be local storage */
		frame = &_frame;
		if (sframe_find(state->ip, frame)) {
			if (!IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP))
				goto done;
			frame = &fp_frame;
		}
	} else if (fp_state(state)) {
		frame = &fp_frame;
	} else {
		goto done;
	}

	if (frame->use_fp) {
		if (state->fp < state->sp)
			goto done;
		cfa = state->fp;
	} else {
		cfa = state->sp;
	}

	/* Get the Canonical Frame Address (CFA) */
	cfa += frame->cfa_off;

	/* stack going in wrong direction? */
	if (cfa <= state->sp)
		goto done;

	/* Make sure that the address is word aligned */
	shift = sizeof(long) == 4 || compat_fp_state(state) ? 2 : 3;
	if ((cfa + frame->ra_off) & ((1 << shift) - 1))
		goto done;

	/* Find the Return Address (RA) */
	if (unwind_get_user_long(ra, cfa + frame->ra_off, state))
		goto done;

	if (frame->fp_off && unwind_get_user_long(fp, cfa + frame->fp_off, state))
		goto done;

	state->ip = ra;
	state->sp = cfa;
	if (frame->fp_off)
		state->fp = fp;

	arch_unwind_user_next(state);

	return 0;

done:
	state->done = true;
	return -EINVAL;
}

static int unwind_user_start(struct unwind_user_state *state)
{
	struct pt_regs *regs = task_pt_regs(current);

	memset(state, 0, sizeof(*state));

	if ((current->flags & PF_KTHREAD) || !user_mode(regs)) {
		state->done = true;
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_HAVE_UNWIND_USER_COMPAT_FP) && in_compat_mode(regs))
		state->type = UNWIND_USER_TYPE_COMPAT_FP;
	else if (current_has_sframe())
		state->type = UNWIND_USER_TYPE_SFRAME;
	else if (IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP))
		state->type = UNWIND_USER_TYPE_FP;
	else
		state->type = UNWIND_USER_TYPE_NONE;

	state->ip = instruction_pointer(regs);
	state->sp = user_stack_pointer(regs);
	state->fp = frame_pointer(regs);

	arch_unwind_user_init(state, regs);

	return 0;
}

int unwind_user(struct unwind_stacktrace *trace, unsigned int max_entries)
{
	struct unwind_user_state state;

	trace->nr = 0;

	if (!max_entries)
		return -EINVAL;

	if (current->flags & PF_KTHREAD)
		return 0;

	for_each_user_frame(&state) {
		trace->entries[trace->nr++] = state.ip;
		if (trace->nr >= max_entries)
			break;
	}

	return 0;
}
