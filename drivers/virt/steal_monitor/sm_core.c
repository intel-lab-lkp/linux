// SPDX-License-Identifier: GPL-2.0-only
/*
 * Steal time Monitor.
 *
 * Periodically compute steal time. Based on the thresholds either
 * reduce/increase the preferred CPUs which can be made use
 * by the workload to avoid vCPU preemption to an extent possible.
 *
 * Available as module with CONFIG_PREFERRED_CPU=y
 *
 * Copyright (C) 2026 IBM
 * Author: Shrikanth Hegde <sshegde@linux.ibm.com>
 */

#include "sm_core.h"

static int __init steal_monitor_init(void)
{
	pr_info("steal_monitor is enabled\n");
	return 0;
}

static void __exit steal_monitor_exit(void)
{
	pr_info("steal_monitor is disabled\n");

	guard(cpus_read_lock)();
	cpumask_copy(&__cpu_preferred_mask, cpu_active_mask);
}

module_init(steal_monitor_init);
module_exit(steal_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Monitor");
