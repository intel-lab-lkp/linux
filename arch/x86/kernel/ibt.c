// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>
#include <asm/msr.h>
#include <asm/fpu/xstate.h>

static bool user_ibt_enabled(struct task_struct *task)
{
	return task->thread.ibt;
}

bool user_ibt_pop_wait_endbr(struct pt_regs *regs)
{
	struct fpu *fpu = x86_task_fpu(current);
	u64 msrval = 0;

	if (!user_ibt_enabled(current))
		return 0;

#ifdef CONFIG_X86_FRED
	if (cpu_feature_enabled(X86_FEATURE_FRED)) {
		msrval = regs->fred_cs.wfe;
		regs->fred_cs.wfe = 0;
		return !!msrval;
	}
#endif

	fpregs_lock();

	if (!test_thread_flag(TIF_NEED_FPU_LOAD)) {
		if (!rdmsrq_safe(MSR_IA32_U_CET, &msrval))
			wrmsrq(MSR_IA32_U_CET, msrval & ~CET_WAIT_ENDBR);
	} else {
		struct cet_user_state *cet;

		/*
		 * If TIF_NEED_FPU_LOAD and get_xsave_addr() returns zero,
		 * XFEATURE_CET_USER is in init state (cet is not active).
		 * Return zero status.
		 */
		cet = get_xsave_addr(&fpu->fpstate->regs.xsave,
				     XFEATURE_CET_USER);
		if (cet) {
			msrval = cet->user_cet;
			cet->user_cet = msrval & ~CET_WAIT_ENDBR;
		}
	}

	fpregs_unlock();

	return !!(msrval & CET_WAIT_ENDBR);
}

void
user_ibt_restore_wait_endbr(struct pt_regs *regs, bool wait_endbr)
{
	struct fpu *fpu = x86_task_fpu(current);
	u64 msrval = 0;

	if (!user_ibt_enabled(current))
		return;

#ifdef CONFIG_X86_FRED
	if (cpu_feature_enabled(X86_FEATURE_FRED)) {
		regs->fred_cs.wfe = wait_endbr;
		return;
	}
#endif

	if (!wait_endbr)
		return;

	fpregs_lock();

	if (!test_thread_flag(TIF_NEED_FPU_LOAD)) {
		if (!rdmsrq_safe(MSR_IA32_U_CET, &msrval))
			wrmsrq(MSR_IA32_U_CET, msrval | CET_WAIT_ENDBR);
	} else {
		struct cet_user_state *cet;

		cet = get_xsave_addr(&fpu->fpstate->regs.xsave,
				     XFEATURE_CET_USER);
		if (cet)
			cet->user_cet |= CET_WAIT_ENDBR;
	}

	fpregs_unlock();
}
