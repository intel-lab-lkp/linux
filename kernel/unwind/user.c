// SPDX-License-Identifier: GPL-2.0
/*
* Generic interface for unwinding user space
*
* Copyright (C) 2024 Josh Poimboeuf <jpoimboe@kernel.org>
*/
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
	case USER_UNWIND_TYPE_SFRAME:
		ret = sframe_find(state->ip, frame);
		if (ret)
			goto the_end;
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
	bool sframe_possible = current_has_sframe();

	memset(state, 0, sizeof(*state));

	if (!current->mm) {
		state->done = true;
		return -EINVAL;
	}

	switch (type) {
	case USER_UNWIND_TYPE_AUTO:
		state->type = sframe_possible ? USER_UNWIND_TYPE_SFRAME :
						USER_UNWIND_TYPE_FP;
		break;
	case USER_UNWIND_TYPE_SFRAME:
		if (!sframe_possible)
			return -EINVAL;
		break;
	case USER_UNWIND_TYPE_FP:
		break;
	default:
		return -EINVAL;
	}

	state->sp = user_stack_pointer(regs);
	state->ip = instruction_pointer(regs);
	state->fp = frame_pointer(regs);

	return user_unwind_next(state);
}
