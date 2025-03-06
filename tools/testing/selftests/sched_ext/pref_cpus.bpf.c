// SPDX-License-Identifier: GPL-2.0
/*
 * A scheduler that validates the behavior of scx_bpf_select_cpu_pref() by
 * selecting idle CPUs strictly within a subset of preferred CPUs.
 *
 * Copyright (c) 2025 Andrea Righi <arighi@nvidia.com>
 */

#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

const volatile unsigned int __COMPAT_SCX_PICK_IDLE_IN_PREF;

private(PREF_CPUS) struct bpf_cpumask __kptr * preferred_cpumask;

s32 BPF_STRUCT_OPS(pref_cpus_select_cpu,
		   struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	const struct cpumask *preferred;
	s32 cpu;

	preferred = cast_mask(preferred_cpumask);
	if (!preferred) {
		scx_bpf_error("preferred domain not initialized");
		return -EINVAL;
	}

	/*
	 * Select an idle CPU strictly within the preferred domain.
	 */
	cpu = scx_bpf_select_cpu_pref(p, preferred, prev_cpu, wake_flags,
				      __COMPAT_SCX_PICK_IDLE_IN_PREF);
	if (cpu >= 0) {
		if (scx_bpf_test_and_clear_cpu_idle(cpu))
			scx_bpf_error("CPU %d should be marked as busy", cpu);

		if (__COMPAT_SCX_PICK_IDLE_IN_PREF &&
		    bpf_cpumask_subset(preferred, p->cpus_ptr) &&
		    !bpf_cpumask_test_cpu(cpu, preferred))
			scx_bpf_error("CPU %d not in the preferred domain for %d (%s)",
				      cpu, p->pid, p->comm);

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);

		return cpu;
	}

	return prev_cpu;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(pref_cpus_init)
{
	struct bpf_cpumask *mask;

	mask = bpf_cpumask_create();
	if (!mask)
		return -ENOMEM;

	mask = bpf_kptr_xchg(&preferred_cpumask, mask);
	if (mask)
		bpf_cpumask_release(mask);

	bpf_rcu_read_lock();

	/*
	 * Assign the first online CPU to the preferred domain.
	 */
	mask = preferred_cpumask;
	if (mask) {
		const struct cpumask *online = scx_bpf_get_online_cpumask();

		bpf_cpumask_set_cpu(bpf_cpumask_first(online), mask);
		scx_bpf_put_cpumask(online);
	}

	bpf_rcu_read_unlock();

	return 0;
}

void BPF_STRUCT_OPS(pref_cpus_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SEC(".struct_ops.link")
struct sched_ext_ops pref_cpus_ops = {
	.select_cpu		= (void *)pref_cpus_select_cpu,
	.init			= (void *)pref_cpus_init,
	.exit			= (void *)pref_cpus_exit,
	.name			= "pref_cpus",
};
