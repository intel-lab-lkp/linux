// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/user_unwind.h>
#include <linux/sframe.h>
#include <linux/uaccess.h>
#include <asm/user_unwind.h>

static struct user_unwind_frame fp_frame = {
	ARCH_INIT_USER_FP_FRAME
};

int user_unwind_next(struct user_unwind_state *state)
{
	struct user_unwind_frame _frame;
	struct user_unwind_frame *frame = &_frame;
	unsigned long cfa, fp, ra;
	int ret = -EINVAL;

	if (state->done)
		return -EINVAL;

	switch (state->type) {
	case USER_UNWIND_TYPE_FP:
		frame = &fp_frame;
		break;
	default:
		BUG();
	}

	cfa = (frame->use_fp ? state->fp : state->sp) + frame->cfa_off;

	if (frame->ra_off && get_user(ra, (unsigned long *)(cfa + frame->ra_off)))
		goto the_end;

	if (frame->fp_off && get_user(fp, (unsigned long *)(cfa + frame->fp_off)))
		goto the_end;

	state->sp = cfa;
	state->ip = ra;
	if (frame->fp_off)
		state->fp = fp;

	return 0;

the_end:
	state->done = true;
	return ret;
}

int user_unwind_start(struct user_unwind_state *state,
		      enum user_unwind_type type)
{
	struct pt_regs *regs = task_pt_regs(current);

	might_sleep();

	memset(state, 0, sizeof(*state));

	if (!current->mm) {
		state->done = true;
		return -EINVAL;
	}

	if (type == USER_UNWIND_TYPE_AUTO)
		state->type = USER_UNWIND_TYPE_FP;
	else
		state->type = type;

	state->sp = user_stack_pointer(regs);
	state->ip = instruction_pointer(regs);
	state->fp = frame_pointer(regs);

	return user_unwind_next(state);
}
