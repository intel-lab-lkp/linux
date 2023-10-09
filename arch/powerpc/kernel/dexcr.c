#include <linux/capability.h>
#include <linux/init.h>
#include <linux/prctl.h>
#include <linux/sched.h>

#include <asm/cpu_has_feature.h>
#include <asm/cputable.h>
#include <asm/processor.h>
#include <asm/reg.h>

/* Allow thread local configuration of these by default */
#define DEXCR_PRCTL_EDITABLE ( \
	DEXCR_PR_IBRTPD | \
	DEXCR_PR_SRAPD | \
	DEXCR_PR_NPHIE)

static unsigned long dexcr_supported __ro_after_init = 0;

static int __init dexcr_init(void)
{
	if (!early_cpu_has_feature(CPU_FTR_ARCH_31))
		return 0;

	if (early_cpu_has_feature(CPU_FTR_DEXCR_SBHE))
		dexcr_supported |= DEXCR_PR_SBHE;

	if (early_cpu_has_feature(CPU_FTR_DEXCR_IBRTPD))
		dexcr_supported |= DEXCR_PR_IBRTPD;

	if (early_cpu_has_feature(CPU_FTR_DEXCR_SRAPD))
		dexcr_supported |= DEXCR_PR_SRAPD;

	if (early_cpu_has_feature(CPU_FTR_DEXCR_NPHIE))
		dexcr_supported |= DEXCR_PR_NPHIE;

	return 0;
}
early_initcall(dexcr_init);

static int prctl_to_aspect(unsigned long which, unsigned int *aspect)
{
	switch (which) {
	case PR_PPC_DEXCR_SBHE:
		*aspect = DEXCR_PR_SBHE;
		break;
	case PR_PPC_DEXCR_IBRTPD:
		*aspect = DEXCR_PR_IBRTPD;
		break;
	case PR_PPC_DEXCR_SRAPD:
		*aspect = DEXCR_PR_SRAPD;
		break;
	case PR_PPC_DEXCR_NPHIE:
		*aspect = DEXCR_PR_NPHIE;
		break;
	default:
		return -ENODEV;
	}

	return 0;
}

int get_dexcr_prctl(struct task_struct *task, unsigned long which)
{
	unsigned int aspect;
	int ret;

	ret = prctl_to_aspect(which, &aspect);
	if (ret)
		return ret;
	
	if (!(aspect & dexcr_supported))
		return -ENODEV;

	if (aspect & task->thread.dexcr_enabled)
		ret |= PR_PPC_DEXCR_CTRL_ON;
	else
		ret |= PR_PPC_DEXCR_CTRL_OFF;

	if (aspect & task->thread.dexcr_inherit)
		ret |= PR_PPC_DEXCR_CTRL_INHERIT;

	return ret;
}

int set_dexcr_prctl(struct task_struct *task, unsigned long which, unsigned long ctrl)
{
	unsigned int aspect;
	unsigned long enable;
	unsigned long disable;
	unsigned long inherit;
	int err = 0;

	/* We do not want an unprivileged process being able to set a value that a setuid process may inherit (particularly for NPHIE) */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	err = prctl_to_aspect(which, &aspect);
	if (err)
		return err;

	if (!(aspect & dexcr_supported))
		return -ENODEV;

	enable = ctrl & PR_PPC_DEXCR_CTRL_ON;
	disable = ctrl & PR_PPC_DEXCR_CTRL_OFF;
	inherit = ctrl & PR_PPC_DEXCR_CTRL_INHERIT;
	ctrl &= ~(PR_PPC_DEXCR_CTRL_ON | PR_PPC_DEXCR_CTRL_OFF | PR_PPC_DEXCR_CTRL_INHERIT);

	if (ctrl)
		return -EINVAL;

	if ((enable && disable) || !(enable || disable))
		return -EINVAL;

	if (enable)
		task->thread.dexcr_enabled |= aspect;
	else
		task->thread.dexcr_enabled &= ~aspect;

	if (inherit)
		task->thread.dexcr_inherit |= aspect;
	else
		task->thread.dexcr_inherit &= ~aspect;

	mtspr(SPRN_DEXCR, get_thread_dexcr(&current->thread));

	return 0;
}
