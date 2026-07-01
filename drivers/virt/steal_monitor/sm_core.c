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

struct steal_monitor sm_core_ctx = {
	.interval_ms = 1000,	/* 1 second */
	.high_threshold = 500,	/* 5% */
	.low_threshold = 200,	/* 2% */
};

module_param_named(interval_ms, sm_core_ctx.interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms,
		 "Sampling frequency for steal values in milliseconds (default: 1000)");

module_param_named(high_threshold, sm_core_ctx.high_threshold, uint, 0644);
MODULE_PARM_DESC(high_threshold,
		 "High steal threshold (default: 500 i.e 5%)");

module_param_named(low_threshold, sm_core_ctx.low_threshold, uint, 0644);
MODULE_PARM_DESC(low_threshold,
		 "Low steal threshold (default: 200 i.e 2%)");

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
