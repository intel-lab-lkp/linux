// SPDX-License-Identifier: GPL-2.0-only
/*
 * Steal time governor driver periodically computes steal time.
 * Based on the thresholds it either reduce/increase the preferred
 * CPUs which can be used by the workload to avoid vCPU preemption
 * to an extent possible in paravirtualized environment.
 *
 * Available as module with CONFIG_STEAL_GOVERNOR
 *
 * Copyright (C) 2026 IBM
 * Author: Shrikanth Hegde <sshegde@linux.ibm.com>
 */

#include "core.h"

#if !IS_ENABLED(CONFIG_PREFERRED_CPU)
#error "Steal Governor requires CONFIG_PREFERRED_CPU"
#endif

static struct steal_governor sg_core_ctx;

static void restore_preferred_to_active(void)
{
	int cpu;

	guard(cpus_read_lock)();
	for_each_cpu(cpu, cpu_active_mask)
		set_cpu_preferred(cpu, true);
}

static int __init steal_governor_init(void)
{
	pr_info("steal_governor is enabled\n");
	return 0;
}

static void __exit steal_governor_exit(void)
{
	restore_preferred_to_active();
	pr_info("steal_governor is disabled\n");
}

module_init(steal_governor_init);
module_exit(steal_governor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Governor");
