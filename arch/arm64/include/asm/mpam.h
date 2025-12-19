/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Arm Ltd. */

#ifndef __ASM__MPAM_H
#define __ASM__MPAM_H

#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/sched.h>

#include <asm/sysreg.h>

DECLARE_STATIC_KEY_FALSE(mpam_enabled);
DECLARE_PER_CPU(u64, arm64_mpam_default);
DECLARE_PER_CPU(u64, arm64_mpam_current);

/*
 * The value of the MPAM0_EL1 sysreg when a task is in resctrl's default group.
 * This is used by the context switch code to use the resctrl CPU property
 * instead. The value is modified when CDP is enabled/disabled by mounting
 * the resctrl filesystem.
 */
extern u64 arm64_mpam_global_default;

/*
 * The resctrl filesystem writes to the partid/pmg values for threads and CPUs,
 * which may race with reads in mpam_thread_switch(). Ensure only one of the old
 * or new values are used. Particular care should be taken with the pmg field as
 * mpam_thread_switch() may read a partid and pmg that don't match, causing this
 * value to be stored with cache allocations, despite being considered 'free' by
 * resctrl.
 *
 * A value in struct thread_info is used instead of struct task_struct as the
 * cpu's u64 register format is used. In struct task_struct there are two u32,
 * rmid and closid for the x86 case, but as we can't use them here do something
 * else. Creating a union would mean only accesses from the created u64 would be
 * endian safe and so be less clear.
 */
static inline u64 mpam_get_regval(struct task_struct *tsk)
{
#ifdef CONFIG_ARM64_MPAM
	return READ_ONCE(task_thread_info(tsk)->mpam_partid_pmg);
#else
	return 0;
#endif
}

static inline void mpam_thread_switch(struct task_struct *tsk)
{
	u64 oldregval;
	int cpu = smp_processor_id();
	u64 regval = mpam_get_regval(tsk);

	if (!IS_ENABLED(CONFIG_ARM64_MPAM) ||
	    !static_branch_likely(&mpam_enabled))
		return;

	if (regval == READ_ONCE(arm64_mpam_global_default))
		regval = READ_ONCE(per_cpu(arm64_mpam_default, cpu));

	oldregval = READ_ONCE(per_cpu(arm64_mpam_current, cpu));
	if (oldregval == regval)
		return;

	write_sysreg_s(regval, SYS_MPAM1_EL1);
	if (system_supports_sme())
		write_sysreg_s(regval & (MPAMSM_EL1_PARTID_D | MPAMSM_EL1_PMG_D), SYS_MPAMSM_EL1);
	isb();

	/* Synchronising the EL0 write is left until the ERET to EL0 */
	write_sysreg_s(regval, SYS_MPAM0_EL1);

	WRITE_ONCE(per_cpu(arm64_mpam_current, cpu), regval);
}
#endif /* __ASM__MPAM_H */
