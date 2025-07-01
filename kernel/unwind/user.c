// SPDX-License-Identifier: GPL-2.0
/*
* Generic interfaces for unwinding user space
*/
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/unwind_user.h>

#define for_each_user_frame(state) \
	for (unwind_user_start(state); !(state)->done; unwind_user_next(state))

static int unwind_user_next(struct unwind_user_state *state)
{
	/* no implementation yet */
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

	state->type = UNWIND_USER_TYPE_NONE;

	state->ip = instruction_pointer(regs);
	state->sp = user_stack_pointer(regs);
	state->fp = frame_pointer(regs);

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
