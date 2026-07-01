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
