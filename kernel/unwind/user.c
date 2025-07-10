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

#ifdef CONFIG_HAVE_UNWIND_USER_COMPAT_FP
static struct unwind_user_frame compat_fp_frame = {
	ARCH_INIT_USER_COMPAT_FP_FRAME
};
#endif

static struct unwind_user_frame *get_fp_frame(struct pt_regs *regs)
{
#ifdef CONFIG_HAVE_UNWIND_USER_COMPAT_FP
	if (unwind_compat_mode(regs))
		return &compat_fp_frame;
#endif
	return &fp_frame;
}

#define for_each_user_frame(state) \
	for (unwind_user_start(state); !(state)->done; unwind_user_next(state))

#define unwind_get_user_long(to, from, regs)				\
({									\
	int __ret;							\
	if (unwind_compat_mode(regs))					\
		__ret = get_user(to, (u32 __user *)(from));		\
	else								\
		__ret = get_user(to, (unsigned long __user *)(from));	\
	__ret;								\
})

static int unwind_user_next_common(struct unwind_user_state *state, struct unwind_user_frame *frame,
				   struct pt_regs *regs)
{
	unsigned long cfa, fp, ra = 0;
	unsigned int shift;

	if (frame->use_fp) {
		if (state->fp < state->sp)
			return -EINVAL;
		cfa = state->fp;
	} else {
		cfa = state->sp;
	}

	/* Get the Canonical Frame Address (CFA) */
	cfa += frame->cfa_off;

	/* stack going in wrong direction? */
	if (cfa <= state->sp)
		return -EINVAL;

	/* Make sure that the address is word aligned */
	shift = (sizeof(long) == 4 || unwind_compat_mode(regs)) ? 2 : 3;
	if ((cfa + frame->ra_off) & ((1 << shift) - 1))
		return -EINVAL;

	/* Find the Return Address (RA) */
	if (unwind_get_user_long(ra, cfa + frame->ra_off, regs))
		return -EINVAL;

	if (frame->fp_off && unwind_get_user_long(fp, cfa + frame->fp_off, regs))
		return -EINVAL;

	state->ip = ra;
	state->sp = cfa;
	if (frame->fp_off)
		state->fp = fp;
	return 0;
}

static int unwind_user_next_sframe(struct unwind_user_state *state)
{
	struct unwind_user_frame _frame, *frame;

	/* sframe expects the frame to be local storage */
	frame = &_frame;
	if (sframe_find(state->ip, frame))
		return -ENOENT;
	return unwind_user_next_common(state, frame, task_pt_regs(current));
}

static int unwind_user_next_fp(struct unwind_user_state *state)
{
	struct pt_regs *regs = task_pt_regs(current);

	return unwind_user_next_common(state, get_fp_frame(regs), regs);
}

static int unwind_user_next(struct unwind_user_state *state)
{
	unsigned long iter_mask = state->available_types;
	unsigned int bit;

	if (state->done)
		return -EINVAL;

	for_each_set_bit(bit, &iter_mask, _NR_UNWIND_USER_TYPE_BITS) {
		enum unwind_user_type type = 1U << bit;

		state->current_type = type;
		switch (type) {
		case UNWIND_USER_TYPE_SFRAME:
			switch (unwind_user_next_sframe(state)) {
			case 0:
				goto end;
			case -ENOENT:
				continue;	/* Try next method. */
			default:
				goto done;
			}
		case UNWIND_USER_TYPE_FP:
			if (!unwind_user_next_fp(state))
				goto end;
			else
				goto done;
		case UNWIND_USER_TYPE_NONE:
			break;
		}
	}

	/* No successful unwind method. */
	goto done;

end:
	arch_unwind_user_next(state);
	return 0;

done:
	state->current_type = UNWIND_USER_TYPE_NONE;
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

	if (current_has_sframe())
		state->available_types |= UNWIND_USER_TYPE_SFRAME;
	if (IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP))
		state->available_types |= UNWIND_USER_TYPE_FP;

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
EXPORT_SYMBOL_GPL(unwind_user);
