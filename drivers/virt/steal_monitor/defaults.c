// SPDX-License-Identifier: GPL-2.0-only
/*
 * Base file contains the default implementations.
 * These are defined as __weak so that arch may define
 * strong symbols to override.
 *
 * Copyright (C) 2026 IBM
 * Author: Shrikanth Hegde <sshegde@linux.ibm.com>
 */
#include "sm_core.h"

/*
 * Compute steal time of the full system.
 *
 * Default implementation returns steal time across all active CPUs
 */

u64 __weak get_system_steal_time(void)
{
	int tmp_cpu;
	u64 total_steal = 0;

	guard(cpus_read_lock)();
	for_each_cpu(tmp_cpu, cpu_active_mask)
		total_steal += kcpustat_cpu(tmp_cpu).cpustat[CPUTIME_STEAL];

	return total_steal;
}

/*
 * Return number of CPUs to consider for steal ratio calculation
 *
 * Default returns number of active CPUs.
 */
unsigned int __weak get_num_cpus_steal_ratio(void)
{
	guard(cpus_read_lock)();
	return num_active_cpus();
}

/*
 * Default implementation of decrementing the preferred CPUs based on steal
 * time. This is simple logic and decrease the preferred CPUs by 1 core.
 * It takes out the last core in the active & preferred.
 *
 * Ensure at least one housekeeping core is always kept as preferred
 *
 * Could be overwritten by arch specific handling. Arch must ensure
 * preferred is always subset of active.
 */

#define get_core_mask(cpu) topology_sibling_cpumask(cpu)

void __weak decrease_preferred_cpus(struct steal_monitor *ctx)
{
	int tmp_cpu, first_hk_cpu;
	const struct cpumask *first_hk_core;
	int target_cpu = nr_cpu_ids;

	guard(cpus_read_lock)();

	first_hk_cpu = cpumask_first_and(housekeeping_cpumask(HK_TYPE_KERNEL_NOISE),
					 cpu_active_mask);

	if (first_hk_cpu >= nr_cpu_ids)
		return;

	first_hk_core = get_core_mask(first_hk_cpu);

	/* Always leave first housekeeping core as preferred. */
	for_each_cpu_andnot(tmp_cpu, cpu_preferred_mask, first_hk_core)
		target_cpu = tmp_cpu;

	/* Only the first housekeeping core remains */
	if (target_cpu >= nr_cpu_ids)
		return;

	/*
	 * set tick bit for nohz_full CPU to push the task out. Once the tasks
	 * are pushed out, bit will be cleared if there are no tasks.
	 */

	for_each_cpu_and(tmp_cpu, get_core_mask(target_cpu), cpu_active_mask) {
		set_cpu_preferred(tmp_cpu, false);
		if (tick_nohz_full_cpu(tmp_cpu))
			tick_nohz_dep_set_cpu(tmp_cpu, TICK_DEP_BIT_SCHED);
	}
}

/*
 * Default implementation of incrementing preferred CPUs based on steal
 * time. This is simple logic and increases the preferred CPUs by 1 core.
 * It adds the first core in active & !preferred
 *
 * Nothing to do if active == preferred
 *
 * Could be overwritten by arch specific handling. Arch must ensure
 * preferred is subset of active.
 */
void __weak increase_preferred_cpus(struct steal_monitor *ctx)
{
	int first_cpu, tmp_cpu;

	guard(cpus_read_lock)();

	first_cpu = cpumask_first_andnot(cpu_active_mask, cpu_preferred_mask);
	/* All CPUs are preferred. Nothing to increase further */
	if (first_cpu >= nr_cpu_ids)
		return;

	for_each_cpu_and(tmp_cpu, get_core_mask(first_cpu), cpu_active_mask)
		set_cpu_preferred(tmp_cpu, true);
}
