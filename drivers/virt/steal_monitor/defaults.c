// SPDX-License-Identifier: GPL-2.0-only
/*
 * Base file contains the default implementations.
 *
 * Copyright (C) 2026 IBM
 * Author: Shrikanth Hegde <sshegde@linux.ibm.com>
 */
#include "sm_core.h"

/*
 * Returns steal time of the full system.
 * Compute collective steal time across all possible CPUs.
 */
u64 get_system_steal_time(void)
{
	int cpu;
	u64 total_steal = 0;

	for_each_possible_cpu(cpu)
		total_steal += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];

	return total_steal;
}

/*
 * Returns number of CPUs to consider for steal ratio.
 * Return possible CPUs.
 */
unsigned int get_num_cpus_steal_ratio(void)
{
	return num_possible_cpus();
}

/*
 * Take action to decrease preferred CPUs.
 *
 * Decrease the preferred CPUs by 1 core.
 * Take out the last core in the active & preferred.
 *
 * Must ensure
 * - least one housekeeping core is always kept as preferred
 * - preferred is always subset of active.
 */
void decrease_preferred_cpus(struct steal_monitor *ctx)
{
	int tmp_cpu, first_hk_cpu, last_cpu;
	const struct cpumask *first_hk_core;
	int target_cpu = nr_cpu_ids;

	guard(cpus_read_lock)();
	first_hk_cpu = cpumask_first_and(housekeeping_cpumask(HK_TYPE_KERNEL_NOISE),
					 cpu_preferred_mask);
	last_cpu = cpumask_last(cpu_preferred_mask);

	if (first_hk_cpu >= nr_cpu_ids || last_cpu >= nr_cpu_ids)
		return;

	/* Always leave first housekeeping core as preferred. */
	first_hk_core = topology_sibling_cpumask(first_hk_cpu);

	/* Find the last CPU which doesn't belong to that first hk_core. */
	if (!cpumask_test_cpu(last_cpu, first_hk_core)) {
		target_cpu = last_cpu;
	} else {
		for_each_cpu_andnot(tmp_cpu, cpu_preferred_mask, first_hk_core)
			target_cpu = tmp_cpu;
	}

	/* Only the first housekeeping core remains */
	if (target_cpu >= nr_cpu_ids)
		return;

	for_each_cpu_and(tmp_cpu, topology_sibling_cpumask(target_cpu),
			 cpu_preferred_mask)
		set_cpu_preferred(tmp_cpu, false);
}

/*
 * Take action to increase preferred CPUs.
 *
 * Increase the preferred CPUs by 1 core.
 * Add the first core in active & !preferred
 *
 * Must ensure preferred is subset of active.
 */
void increase_preferred_cpus(struct steal_monitor *ctx)
{
	int first_cpu, tmp_cpu;

	guard(cpus_read_lock)();
	first_cpu = cpumask_first_andnot(cpu_active_mask, cpu_preferred_mask);

	/* All CPUs are preferred. Nothing to increase further */
	if (first_cpu >= nr_cpu_ids)
		return;

	for_each_cpu_and(tmp_cpu, topology_sibling_cpumask(first_cpu),
			 cpu_active_mask)
		set_cpu_preferred(tmp_cpu, true);
}
