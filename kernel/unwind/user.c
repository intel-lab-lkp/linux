// SPDX-License-Identifier: GPL-2.0
/*
* Generic interfaces for unwinding user space
*/

#define pr_fmt(fmt)	"unwind_user: " fmt

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/unwind_user.h>
#include <linux/uaccess.h>
#include <linux/eh_frame.h>

#ifdef CONFIG_DYNAMIC_DEBUG

#define dbg_once(fmt, ...)							\
	pr_debug_once("%s (%d): " fmt, current->comm, current->pid, ##__VA_ARGS__)

#else /* !CONFIG_DYNAMIC_DEBUG */

#define dbg_once(args...)		no_printk(args)

#endif /* !CONFIG_DYNAMIC_DEBUG */

#define for_each_user_frame(state) \
	for (unwind_user_start(state); !(state)->done; unwind_user_next(state))

static inline int
get_user_word(unsigned long *word, unsigned long base, int off, unsigned int ws)
{
	unsigned long __user *addr = (void __user *)base + off;
#ifdef CONFIG_COMPAT
	if (ws == sizeof(int)) {
		unsigned int data;
		int ret = get_user(data, (unsigned int __user *)addr);
		*word = data;
		return ret;
	}
#endif
	return get_user(*word, addr);
}

static int unwind_user_next_common(struct unwind_user_state *state,
				   const struct unwind_user_frame *frame)
{
	unsigned long cfa, sp, fp, ra;

	/* Stop unwinding when reaching an outermost frame. */
	if (frame->outermost) {
		state->done = true;
		return 0;
	}

	/* Get the Canonical Frame Address (CFA) */
	switch (frame->cfa.rule) {
	case UNWIND_USER_CFA_RULE_SP_OFFSET:
	case UNWIND_USER_CFA_RULE_SP_OFFSET_DEREF:
		cfa = state->sp;
		break;
	case UNWIND_USER_CFA_RULE_FP_OFFSET:
	case UNWIND_USER_CFA_RULE_FP_OFFSET_DEREF:
		if (state->fp < state->sp)
			return -EINVAL;
		cfa = state->fp;
		break;
	case UNWIND_USER_CFA_RULE_REG_OFFSET:
	case UNWIND_USER_CFA_RULE_REG_OFFSET_DEREF:
		if (!state->topmost || unwind_user_get_reg(&cfa, frame->cfa.regnum))
			return -EINVAL;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	cfa += frame->cfa.offset;
	if (frame->cfa.rule & UNWIND_USER_RULE_DEREF &&
	    get_user_word(&cfa, cfa, 0, state->ws))
		return -EINVAL;

	/* Get the Stack Pointer (SP) */
	sp = cfa + frame->sp_off;
	/*
	 * Make sure that stack is not going in wrong direction.  Allow SP
	 * to be unchanged for the topmost frame, by subtracting topmost,
	 * which is either 0 or 1.
	 */
	if (sp <= state->sp - state->topmost)
		return -EINVAL;
	/* Make sure that the address is word aligned */
	if (sp & (state->ws - 1))
		return -EINVAL;

	/* Get the Return Address (RA) */
	switch (frame->ra.rule) {
	case UNWIND_USER_RULE_RETAIN:
		if (!state->topmost || unwind_user_get_ra_reg(&ra))
			return -EINVAL;
		break;
	case UNWIND_USER_RULE_CFA_OFFSET:
		/*
		 * RA = CFA + offset does not make sense.
		 * A return address cannot legitimately be a stack address.
		 */
		dbg_once("UNWIND_USER_RULE_CFA_OFFSET invalid for RA\n");
		return -EINVAL;
	case UNWIND_USER_RULE_CFA_OFFSET_DEREF:
		ra = cfa + frame->ra.offset;
		break;
	case UNWIND_USER_RULE_REG_OFFSET:
	case UNWIND_USER_RULE_REG_OFFSET_DEREF:
		if (!state->topmost || unwind_user_get_reg(&ra, frame->ra.regnum))
			return -EINVAL;
		ra += frame->ra.offset;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	if (frame->ra.rule & UNWIND_USER_RULE_DEREF &&
	    get_user_word(&ra, ra, 0, state->ws))
		return -EINVAL;

	/* Get the Frame Pointer (FP) */
	switch (frame->fp.rule) {
	case UNWIND_USER_RULE_RETAIN:
		fp = state->fp;
		break;
	case UNWIND_USER_RULE_CFA_OFFSET:
		/*
		 * FP = CFA + offset is currently not used for FP
		 * (e.g. SFrame cannot represent this rule).
		 */
		dbg_once("UNWIND_USER_RULE_CFA_OFFSET unsupported for FP\n");
		return -EINVAL;
	case UNWIND_USER_RULE_CFA_OFFSET_DEREF:
		fp = cfa + frame->fp.offset;
		break;
	case UNWIND_USER_RULE_REG_OFFSET:
	case UNWIND_USER_RULE_REG_OFFSET_DEREF:
		if (!state->topmost || unwind_user_get_reg(&fp, frame->fp.regnum))
			return -EINVAL;
		fp += frame->fp.offset;
		break;
	default:
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	if (frame->fp.rule & UNWIND_USER_RULE_DEREF &&
	    get_user_word(&fp, fp, 0, state->ws))
		return -EINVAL;

	state->ip = ra;
	state->sp = sp;
	state->fp = fp;
	state->topmost = false;
	return 0;
}

static int unwind_user_next_fp(struct unwind_user_state *state)
{
	struct pt_regs *regs = task_pt_regs(current);

	if (state->topmost && unwind_user_at_function_start(regs)) {
		const struct unwind_user_frame fp_entry_frame = {
			ARCH_INIT_USER_FP_ENTRY_FRAME(state->ws)
		};
		return unwind_user_next_common(state, &fp_entry_frame);
	}

	const struct unwind_user_frame fp_frame = {
		ARCH_INIT_USER_FP_FRAME(state->ws)
	};
	return unwind_user_next_common(state, &fp_frame);
}

static int unwind_user_next_eh_frame(struct unwind_user_state *state)
{
	struct unwind_user_frame frame;

	/* eh_frame expects the frame to be local storage */
	if (eh_frame_find(state->ip, &frame))
		return -ENOENT;
	return unwind_user_next_common(state, &frame);
}

static int unwind_user_next(struct unwind_user_state *state)
{
	unsigned long iter_mask = state->available_types;
	unsigned int bit;

	if (state->done)
		return -EINVAL;

	for_each_set_bit(bit, &iter_mask, NR_UNWIND_USER_TYPE_BITS) {
		enum unwind_user_type type = BIT(bit);

		state->current_type = type;
		switch (type) {
		case UNWIND_USER_TYPE_EH_FRAME:
			switch (unwind_user_next_eh_frame(state)) {
			case 0:
				return 0;
			case -ENOENT:
				continue;	/* Try next method. */
			default:
				state->done = true;
			}
			break;
		case UNWIND_USER_TYPE_FP:
			if (!unwind_user_next_fp(state))
				return 0;
			continue;
		default:
			WARN_ONCE(1, "Undefined unwind bit %d", bit);
			break;
		}
		break;
	}

	/* No successful unwind method. */
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

	if (current_has_eh_frame())
		state->available_types |= UNWIND_USER_TYPE_EH_FRAME;
	if (IS_ENABLED(CONFIG_HAVE_UNWIND_USER_FP))
		state->available_types |= UNWIND_USER_TYPE_FP;

	state->ip = instruction_pointer(regs);
	state->sp = user_stack_pointer(regs);
	state->fp = frame_pointer(regs);
	state->ws = unwind_user_word_size(regs);
	if (!state->ws) {
		state->done = true;
		return -EINVAL;
	}
	state->topmost = true;

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
