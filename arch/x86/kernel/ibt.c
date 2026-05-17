// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>
#include <linux/cpu.h>
#include <linux/prctl.h>
#include <asm/msr.h>
#include <asm/fpu/xstate.h>

static bool user_ibt_enabled(struct task_struct *task)
{
	return task->thread.ibt;
}

static bool user_ibt_locked(struct task_struct *task)
{
	return task->thread.ibt_locked;
}

static void user_ibt_set_lock(struct task_struct *task, bool lock)
{
	task->thread.ibt_locked = lock;
}

static void user_ibt_set_enable(bool enable)
{
	u64 msrval;

	/* Already enabled */
	if (user_ibt_enabled(current) == enable)
		return;

	current->thread.ibt = !!enable;

	fpregs_lock_and_load();
	rdmsrq(MSR_IA32_U_CET, msrval);
	if (enable)
		msrval |= CET_ENDBR_EN | CET_NO_TRACK_EN;
	else
		msrval &= ~(CET_ENDBR_EN | CET_NO_TRACK_EN);
	msrval &= ~CET_WAIT_ENDBR;
	wrmsrq(MSR_IA32_U_CET, msrval);
	fpregs_unlock();
}

int arch_prctl_get_branch_landing_pad_state(struct task_struct *t,
					    unsigned long __user *state)
{
	unsigned long status = 0;

	if (!cpu_feature_enabled(X86_FEATURE_USER_IBT) || in_ia32_syscall())
		return -EINVAL;

	status = (user_ibt_enabled(t) ? PR_CFI_ENABLE : PR_CFI_DISABLE);
	status |= (user_ibt_locked(t) ? PR_CFI_LOCK : 0);

	return copy_to_user(state, &status, sizeof(status)) ? -EFAULT : 0;
}

int arch_prctl_set_branch_landing_pad_state(struct task_struct *t, unsigned long state)
{
	if (!cpu_feature_enabled(X86_FEATURE_USER_IBT) || in_ia32_syscall())
		return -EINVAL;

	if (t != current)
		return -EINVAL;

	if (user_ibt_locked(t))
		return -EINVAL;

	if (!(state & (PR_CFI_ENABLE | PR_CFI_DISABLE)))
		return -EINVAL;

	if (state & PR_CFI_ENABLE && state & PR_CFI_DISABLE)
		return -EINVAL;

	user_ibt_set_enable(!!(state & PR_CFI_ENABLE));

	return 0;
}

int arch_prctl_lock_branch_landing_pad_state(struct task_struct *task)
{
	if (!cpu_feature_enabled(X86_FEATURE_USER_IBT) ||
	    !user_ibt_enabled(task) ||
	    in_ia32_syscall())
		return -EINVAL;

	user_ibt_set_lock(task, true);

	return 0;
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

void reset_thread_ibt(void)
{
	current->thread.ibt = false;
	current->thread.ibt_locked = false;
}
