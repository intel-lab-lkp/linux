// SPDX-License-Identifier: GPL-2.0-only
/*
 * Steal time Monitor.
 *
 * Periodically compute steal time. Based on the thresholds either
 * reduce/increase the preferred CPUs which can be used
 * by the workload to avoid vCPU preemption to an extent possible.
 *
 * Available as module with CONFIG_STEAL_MONITOR=m
 *
 * Copyright (C) 2026 IBM
 * Author: Shrikanth Hegde <sshegde@linux.ibm.com>
 */

#include "sm_core.h"

struct steal_monitor sm_core_ctx;

static int __init steal_monitor_init(void)
{
	pr_info("steal_monitor is enabled\n");
	return 0;
}

static void __exit steal_monitor_exit(void)
{
	guard(cpus_read_lock)();
	cpumask_copy(&__cpu_preferred_mask, cpu_active_mask);

	pr_info("steal_monitor is disabled\n");
}

module_init(steal_monitor_init);
module_exit(steal_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Monitor");
