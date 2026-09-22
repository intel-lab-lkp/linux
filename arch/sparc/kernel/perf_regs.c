// SPDX-License-Identifier: GPL-2.0
#include <linux/perf_event.h>
#include <linux/perf_regs.h>
#include <linux/sched/task_stack.h>
#include <asm/cacheflush.h>
#include <asm/ptrace.h>

u64 perf_reg_value(struct pt_regs *regs, int idx)
{
	u64 val;

	switch (idx) {
	case PERF_REG_SPARC_G0 ... PERF_REG_SPARC_O7:
		val = regs->u_regs[idx];
		break;
	case PERF_REG_SPARC_TSTATE:
		return regs->tstate;
	case PERF_REG_SPARC_PC:
		val = regs->tpc;
		break;
	case PERF_REG_SPARC_NPC:
		val = regs->tnpc;
		break;
	case PERF_REG_SPARC_Y:
		return regs->y;
	default:
		WARN_ON_ONCE(1);
		return 0;
	}

	if (test_thread_flag(TIF_32BIT))
		val = (u32)val;

	return val;
}

#define REG_RESERVED (~((1ULL << PERF_REG_SPARC_MAX) - 1))

int perf_reg_validate(u64 mask)
{
	if (!mask || mask & REG_RESERVED)
		return -EINVAL;

	return 0;
}

u64 perf_reg_abi(struct task_struct *task)
{
	if (test_tsk_thread_flag(task, TIF_32BIT))
		return PERF_SAMPLE_REGS_ABI_32;

	return PERF_SAMPLE_REGS_ABI_64;
}

void perf_get_regs_user(struct perf_regs *regs_user,
			struct pt_regs *regs)
{
	regs_user->regs = task_pt_regs(current);
	regs_user->abi = perf_reg_abi(current);
}

void perf_arch_prepare_ustack(void)
{
	flushw_user();
}
