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

static void compute_preferred_cpus_work(struct work_struct *work)
{
	u64 curr_steal, delta_steal, delta_ns, steal_ratio;
	ktime_t now;

	curr_steal = get_system_steal_time();
	now = ktime_get();

	/* get the deltas */
	delta_steal = curr_steal > sm_core_ctx.prev_steal ?
		      curr_steal - sm_core_ctx.prev_steal : 0;
	delta_ns = max_t(u64, ktime_to_ns(ktime_sub(now, sm_core_ctx.prev_time)), 1);

	/* Update for next calculation */
	sm_core_ctx.prev_steal = curr_steal;
	sm_core_ctx.prev_time = now;

	/*
	 * Multiply by 100 to consider the fractional values of steal time.
	 * steal_ratio = (delta_steal * 100 * 100)/(delta_ns * num_cpus())
	 */
	delta_ns = div_u64(delta_ns * get_num_cpus_steal_ratio(), 100 * 100);
	if (unlikely(!delta_ns))
		return;

	steal_ratio = div64_u64(delta_steal, delta_ns);
	/* If the steal time values are high, reduce preferred CPUs */
	if (steal_ratio > sm_core_ctx.high_threshold)
		decrease_preferred_cpus(&sm_core_ctx);
	/* If the steal time values are low, increase preferred CPUs */
	if (steal_ratio <= sm_core_ctx.low_threshold)
		increase_preferred_cpus(&sm_core_ctx);

	/* At least one core is kept as preferred */
	WARN_ON(cpumask_empty(cpu_preferred_mask));

	/* Warn if interval_ms is set to 0, that might cause lockup. */
	if (unlikely(sm_core_ctx.interval_ms == 0)) {
		WARN_ON(1);
		sm_core_ctx.interval_ms = 1000; /* Fallback to default */
	}

	/* Trigger for next sampling */
	schedule_delayed_work(&sm_core_ctx.work,
			      msecs_to_jiffies(sm_core_ctx.interval_ms));
}

static int __init steal_monitor_init(void)
{
	pr_info("steal_monitor is enabled. interval: %ums, high_threshold: %u, low_threshold: %u\n",
		sm_core_ctx.interval_ms, sm_core_ctx.high_threshold, sm_core_ctx.low_threshold);

	INIT_DELAYED_WORK(&sm_core_ctx.work, compute_preferred_cpus_work);
	sm_core_ctx.prev_steal = get_system_steal_time();
	sm_core_ctx.prev_time = ktime_get();

	schedule_delayed_work(&sm_core_ctx.work,
			      msecs_to_jiffies(sm_core_ctx.interval_ms));

	return 0;
}

static void __exit steal_monitor_exit(void)
{
	pr_info("steal_monitor is disabled\n");

	cancel_delayed_work_sync(&sm_core_ctx.work);
	guard(cpus_read_lock)();
	cpumask_copy(&__cpu_preferred_mask, cpu_active_mask);
}

module_init(steal_monitor_init);
module_exit(steal_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Monitor");
