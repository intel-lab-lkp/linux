// SPDX-License-Identifier: GPL-2.0-only
#include <linux/objtool.h>
#include <linux/module.h>
#include <linux/orc.h>
#include <linux/bpf.h>
#include <asm/ptrace.h>
#include <asm/stacktrace.h>
#include <asm/unwind.h>
#include <asm/orc_types.h>
#include <asm/orc_lookup.h>
#include <asm/orc_header.h>
#include <generated/user_constants.h>
#include <os.h>

ORC_HEADER;

#define orc_warn(fmt, ...) \
	printk_deferred_once(KERN_WARNING "WARNING: " fmt, ##__VA_ARGS__)

#define orc_warn_current(args...)					\
({									\
	static bool dumped_before;					\
	if (state->task == current && !state->error) {			\
		orc_warn(args);						\
		if (unwind_debug && !dumped_before) {			\
			dumped_before = true;				\
			unwind_dump(state);				\
		}							\
	}								\
})

extern int __start_orc_unwind_ip[];
extern int __stop_orc_unwind_ip[];
extern struct orc_entry __start_orc_unwind[];
extern struct orc_entry __stop_orc_unwind[];

static bool orc_init __ro_after_init;
static bool unwind_debug __ro_after_init;
static unsigned int lookup_num_blocks __ro_after_init;

static int __init unwind_debug_cmdline(char *str)
{
	unwind_debug = true;

	return 0;
}
early_param("unwind_debug", unwind_debug_cmdline);

static void unwind_dump(struct unwind_state *state)
{
	static bool dumped_before;
	unsigned long word, *sp;
	struct stack_info stack_info = {0};
	unsigned long visit_mask = 0;

	if (dumped_before)
		return;

	dumped_before = true;

	printk_deferred("unwind stack type:%d next_sp:%p mask:0x%lx graph_idx:%d\n",
			state->stack_info.type, state->stack_info.next_sp,
			state->stack_mask, state->graph_idx);

	for (sp = __builtin_frame_address(0); sp;
	     sp = PTR_ALIGN(stack_info.next_sp, sizeof(long))) {
		if (get_stack_info(sp, state->task, &stack_info, &visit_mask))
			break;

		for (; sp < stack_info.end; sp++) {
			word = READ_ONCE_NOCHECK(*sp);

			printk_deferred("%0*lx: %0*lx (%pB)\n", BITS_PER_LONG / 4,
					(unsigned long)sp, BITS_PER_LONG / 4,
					word, (void *)word);
		}
	}
}

/* Fake frame pointer entry -- used as a fallback for generated code */
static struct orc_entry orc_fp_entry = {
	.type		= ORC_TYPE_CALL,
	.sp_reg		= ORC_REG_BP,
	.sp_offset	= 16,
	.bp_reg		= ORC_REG_PREV_SP,
	.bp_offset	= -16,
};

static struct orc_entry *orc_bpf_find(unsigned long ip)
{
#ifdef CONFIG_BPF_JIT
	if (bpf_has_frame_pointer(ip))
		return &orc_fp_entry;
#endif

	return NULL;
}

/*
 * If we crash with IP==0, the last successfully executed instruction
 * was probably an indirect function call with a NULL function pointer,
 * and we don't have unwind information for NULL.
 * This hardcoded ORC entry for IP==0 allows us to unwind from a NULL function
 * pointer into its parent and then continue normally from there.
 */
static struct orc_entry null_orc_entry = {
	.sp_offset = sizeof(long),
	.sp_reg = ORC_REG_SP,
	.bp_reg = ORC_REG_UNDEFINED,
	.type = ORC_TYPE_CALL
};

static struct orc_entry *orc_find(unsigned long ip)
{
	struct orc_entry *orc;

	if (ip == 0)
		return &null_orc_entry;

	/* For non-init vmlinux addresses, use the fast lookup table: */
	if (ip >= LOOKUP_START_IP && ip < LOOKUP_STOP_IP) {
		unsigned int idx, start, stop;

		idx = (ip - LOOKUP_START_IP) / LOOKUP_BLOCK_SIZE;

		if (unlikely((idx >= lookup_num_blocks - 1))) {
			orc_warn("bad lookup idx: idx=%u num=%u ip=%pB\n",
				 idx, lookup_num_blocks, (void *)ip);
			return NULL;
		}

		start = orc_lookup[idx];
		stop = orc_lookup[idx + 1] + 1;

		if (unlikely((__start_orc_unwind + start >= __stop_orc_unwind) ||
			     (__start_orc_unwind + stop > __stop_orc_unwind))) {
			orc_warn("bad lookup value: idx=%u num=%u start=%u stop=%u ip=%pB\n",
				 idx, lookup_num_blocks, start, stop, (void *)ip);
			return NULL;
		}

		return __orc_find(__start_orc_unwind_ip + start,
				  __start_orc_unwind + start, stop - start, ip);
	}

	/* vmlinux .init slow lookup: */
	if (is_kernel_inittext(ip))
		return __orc_find(__start_orc_unwind_ip, __start_orc_unwind,
				  __stop_orc_unwind_ip - __start_orc_unwind_ip, ip);

	/* Module lookup: */
	orc = orc_module_find(ip);
	if (orc)
		return orc;

	/* BPF lookup: */
	return orc_bpf_find(ip);
}

void __init unwind_init(void)
{
	size_t orc_ip_size = (void *)__stop_orc_unwind_ip - (void *)__start_orc_unwind_ip;
	size_t orc_size = (void *)__stop_orc_unwind - (void *)__start_orc_unwind;
	size_t num_entries = orc_ip_size / sizeof(int);
	struct orc_entry *orc;
	int i;

	if (!num_entries || orc_ip_size % sizeof(int) != 0 ||
	    orc_size % sizeof(struct orc_entry) != 0 ||
	    num_entries != orc_size / sizeof(struct orc_entry)) {
		orc_warn("Bad or missing .orc_unwind table.  Disabling unwinder.\n");
		return;
	}

	/*
	 * Note, the orc_unwind and orc_unwind_ip tables were already
	 * sorted at build time via the 'sorttable' tool.
	 * It's ready for binary search straight away, no need to sort it.
	 */

	/* Initialize the fast lookup table: */
	lookup_num_blocks = orc_lookup_end - orc_lookup;
	for (i = 0; i < lookup_num_blocks - 1; i++) {
		orc = __orc_find(__start_orc_unwind_ip, __start_orc_unwind,
				 num_entries,
				 LOOKUP_START_IP + (LOOKUP_BLOCK_SIZE * i));
		if (!orc) {
			orc_warn("Corrupt .orc_unwind table.  Disabling unwinder.\n");
			return;
		}

		orc_lookup[i] = orc - __start_orc_unwind;
	}

	/* Initialize the ending block: */
	orc = __orc_find(__start_orc_unwind_ip, __start_orc_unwind, num_entries,
			 LOOKUP_STOP_IP);
	if (!orc) {
		orc_warn("Corrupt .orc_unwind table.  Disabling unwinder.\n");
		return;
	}
	orc_lookup[lookup_num_blocks - 1] = orc - __start_orc_unwind;

	orc_init = true;
}

unsigned long unwind_get_return_address(struct unwind_state *state)
{
	if (unwind_done(state))
		return 0;

	return __kernel_text_address(state->ip) ? state->ip : 0;
}
EXPORT_SYMBOL_GPL(unwind_get_return_address);

static bool stack_access_ok(struct unwind_state *state, unsigned long _addr,
			    size_t len)
{
	struct stack_info *info = &state->stack_info;
	void *addr = (void *)_addr;

	if (on_stack(info, addr, len))
		return true;

	return !get_stack_info(addr, state->task, info, &state->stack_mask) &&
		on_stack(info, addr, len);
}

static bool deref_stack_reg(struct unwind_state *state, unsigned long addr,
			    unsigned long *val)
{
	if (!stack_access_ok(state, addr, sizeof(long)))
		return false;

	*val = READ_ONCE_NOCHECK(*(unsigned long *)addr);
	return true;
}

bool unwind_next_frame(struct unwind_state *state)
{
	unsigned long ip_p, sp, orig_ip = state->ip, prev_sp = state->sp;
	enum stack_type prev_type = state->stack_info.type;
	struct orc_entry *orc;
	bool indirect = false;

	if (unwind_done(state))
		return false;

	/* Don't let modules unload while we're reading their ORC data. */
	guard(rcu)();

	/* End-of-stack check for user tasks: */
	if (state->regs && user_mode(state->regs))
		goto the_end;

	/*
	 * Find the orc_entry associated with the text address.
	 *
	 * For a call frame (as opposed to a signal frame), state->ip points to
	 * the instruction after the call.  That instruction's stack layout
	 * could be different from the call instruction's layout, for example
	 * if the call was to a noreturn function.  So get the ORC data for the
	 * call instruction itself.
	 */
	orc = orc_find(state->signal ? state->ip : state->ip - 1);
	if (!orc) {
		/*
		 * As a fallback, try to assume this code uses a frame pointer.
		 * This is just a guess, so the rest of the unwind is no longer
		 * considered reliable.
		 */
		orc = &orc_fp_entry;
		state->error = true;
	} else {
		if (orc->type == ORC_TYPE_UNDEFINED)
			goto err;

		if (orc->type == ORC_TYPE_END_OF_STACK)
			goto the_end;
	}

	state->signal = orc->signal;

	/* Find the previous frame's stack: */
	switch (orc->sp_reg) {
	case ORC_REG_SP:
		sp = state->sp + orc->sp_offset;
		break;

	case ORC_REG_BP:
		sp = state->bp + orc->sp_offset;
		break;

	case ORC_REG_SP_INDIRECT:
		sp = state->sp;
		indirect = true;
		break;

	case ORC_REG_BP_INDIRECT:
		sp = state->bp + orc->sp_offset;
		indirect = true;
		break;

	default:
		/* thee rest are only used in x86 asm code, not in UML */
		orc_warn("unsupported SP base reg %d at %pB\n",
			 orc->sp_reg, (void *)state->ip);
		goto err;
	}

	if (indirect) {
		if (!deref_stack_reg(state, sp, &sp))
			goto err;

		if (orc->sp_reg == ORC_REG_SP_INDIRECT)
			sp += orc->sp_offset;
	}

	/* Find IP and SP: */
	switch (orc->type) {
	case ORC_TYPE_CALL:
		ip_p = sp - sizeof(long);

		if (!deref_stack_reg(state, ip_p, &state->ip))
			goto err;

		state->ip = unwind_recover_ret_addr(state, state->ip,
						    (unsigned long *)ip_p);
		state->sp = sp;
		state->regs = NULL;
		break;

	default:
		/* also only in x86 asm code */
		orc_warn("unsupported .orc_unwind entry type %d at %pB\n",
			 orc->type, (void *)orig_ip);
		goto err;
	}

	/* Find BP: */
	switch (orc->bp_reg) {
	case ORC_REG_UNDEFINED:
		if (state->regs)
			state->bp = PT_REGS_BP(state->regs);
		break;

	case ORC_REG_PREV_SP:
		if (!deref_stack_reg(state, sp + orc->bp_offset, &state->bp))
			goto err;
		break;

	case ORC_REG_BP:
		if (!deref_stack_reg(state, state->bp + orc->bp_offset, &state->bp))
			goto err;
		break;

	default:
		orc_warn("unknown BP base reg %d for ip %pB\n",
			 orc->bp_reg, (void *)orig_ip);
		goto err;
	}

	/*
	 * If we find sigreturn then we're unwinding across a single, which
	 * means the ucontext of the interrupted code is on the stack too.
	 */
	if (os_sigreturn_ip && state->ip == os_sigreturn_ip) {
		unsigned long uc = state->sp;

		if (!deref_stack_reg(state, uc + HOST_UC_IP, &state->ip) ||
		    !deref_stack_reg(state, uc + HOST_UC_SP, &state->sp) ||
		    !deref_stack_reg(state, uc + HOST_UC_BP, &state->bp))
			goto err;
		state->signal = true;
	}

	/* Prevent a recursive loop due to bad ORC data: */
	if (state->stack_info.type == prev_type &&
	    on_stack(&state->stack_info, (void *)state->sp, sizeof(long)) &&
	    state->sp <= prev_sp) {
		orc_warn_current("stack going in the wrong direction? at %pB\n",
				 (void *)orig_ip);
		goto err;
	}

	return true;

err:
	state->error = true;

the_end:
	state->stack_info.type = STACK_TYPE_UNKNOWN;
	return false;
}
EXPORT_SYMBOL_GPL(unwind_next_frame);

void __unwind_start(struct unwind_state *state, struct task_struct *task,
		    struct pt_regs *regs, unsigned long *first_frame)
{
	memset(state, 0, sizeof(*state));
	state->task = task;

	if (!orc_init)
		goto err;

	/*
	 * Refuse to unwind the stack of a task while it's executing on another
	 * CPU.  This check is racy, but that's ok: the unwinder has other
	 * checks to prevent it from going off the rails.
	 */
	if (task_on_another_cpu(task))
		goto err;

	if (regs) {
		if (user_mode(regs))
			goto the_end;

		state->ip = PT_REGS_IP(regs);
		state->sp = PT_REGS_SP(regs);
		state->bp = PT_REGS_BP(regs);
		state->regs = regs;
		state->signal = true;
	} else if (task == current) {
		/* all three must come from the same instruction */
		asm volatile("lea (%%rip), %0\n\t"
			     "mov %%rsp, %1\n\t"
			     "mov %%rbp, %2\n\t"
			     : "=r" (state->ip), "=r" (state->sp),
			       "=r" (state->bp));
	} else {
		/* registers saved by switch_threads() when it was switched out */
		state->ip = KSTK_EIP(task);
		state->sp = KSTK_ESP(task);
		state->bp = KSTK_EBP(task);
	}

	if (get_stack_info((unsigned long *)state->sp, state->task,
			   &state->stack_info, &state->stack_mask)) {
		/*
		 * We weren't on a valid stack.  It's possible that
		 * we overflowed a valid stack into a guard page.
		 * See if the next page up is valid so that we can
		 * generate some kind of backtrace if this happens.
		 */
		void *next_page = (void *)PAGE_ALIGN((unsigned long)state->sp);

		state->error = true;
		if (get_stack_info(next_page, state->task, &state->stack_info,
				   &state->stack_mask))
			return;
	}

	/*
	 * The caller can provide the address of the first frame directly
	 * (first_frame) or indirectly (regs->sp) to indicate which stack frame
	 * to start unwinding at.  Skip ahead until we reach it.
	 */

	/* When starting from regs, skip the regs frame: */
	if (regs) {
		unwind_next_frame(state);
		return;
	}

	/* Otherwise, skip ahead to the user-specified starting frame: */
	while (!unwind_done(state) &&
	       (!on_stack(&state->stack_info, first_frame, sizeof(long)) ||
			state->sp <= (unsigned long)first_frame))
		unwind_next_frame(state);

	return;

err:
	state->error = true;
the_end:
	state->stack_info.type = STACK_TYPE_UNKNOWN;
}
EXPORT_SYMBOL_GPL(__unwind_start);
