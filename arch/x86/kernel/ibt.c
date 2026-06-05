// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>
#include <linux/cpu.h>
#include <linux/prctl.h>
#include <asm/msr.h>

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

	if (!cpu_feature_enabled(X86_FEATURE_USER_IBT))
		return -EINVAL;

	status = (user_ibt_enabled(t) ? PR_CFI_ENABLE : PR_CFI_DISABLE);
	status |= (user_ibt_locked(t) ? PR_CFI_LOCK : 0);

	return copy_to_user(state, &status, sizeof(status)) ? -EFAULT : 0;
}

int arch_prctl_set_branch_landing_pad_state(struct task_struct *t, unsigned long state)
{
	if (!cpu_feature_enabled(X86_FEATURE_USER_IBT))
		return -EINVAL;

	if (t != current)
		return -EINVAL;

	if (state & ~PR_CFI_SUPPORTED_STATUS_MASK)
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
	    !user_ibt_enabled(task))
		return -EINVAL;

	user_ibt_set_lock(task, true);

	return 0;
}

void reset_thread_ibt(void)
{
	current->thread.ibt = false;
	current->thread.ibt_locked = false;
}
