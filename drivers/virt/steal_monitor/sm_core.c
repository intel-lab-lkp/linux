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

struct steal_monitor sm_core_ctx = {
	.interval_ms = 1000,	/* 1 second */
	.high_threshold = 500,	/* 5% */
	.low_threshold = 200,	/* 2% */
};

static int param_set_interval_ms(const char *val, const struct kernel_param *kp)
{
	unsigned int interval;
	int ret;

	ret = kstrtouint(val, 0, &interval);
	if (ret)
		return ret;

	if (interval < 10 || interval > 100000) {
		pr_err("steal_monitor: interval_ms must be between 10 and 100000\n");
		return -EINVAL;
	}

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops interval_ms_ops = {
	.set = param_set_interval_ms,
	.get = param_get_uint,
};

module_param_cb(interval_ms, &interval_ms_ops, &sm_core_ctx.interval_ms, 0444);
MODULE_PARM_DESC(interval_ms,
		 "Sampling frequency in milliseconds. default: 1000");

static int param_set_high_threshold(const char *val, const struct kernel_param *kp)
{
	unsigned int threshold;
	int ret;

	ret = kstrtouint(val, 0, &threshold);
	if (ret)
		return ret;

	if (threshold <= sm_core_ctx.low_threshold) {
		pr_err("steal_monitor: high_threshold (%u) must be more than low_threshold (%u)\n",
		       threshold, sm_core_ctx.low_threshold);
		return -EINVAL;
	}

	if (threshold >= 100 * 100) {
		pr_err("steal_monitor: high_threshold (%u) can't be more than 99.99%%\n",
		       threshold);
		return -EINVAL;
	}

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops high_threshold_ops = {
	.set = param_set_high_threshold,
	.get = param_get_uint,
};

module_param_cb(high_threshold, &high_threshold_ops, &sm_core_ctx.high_threshold, 0444);
MODULE_PARM_DESC(high_threshold,
		 "High steal threshold. default: 500 i.e 5%. Must be > low_threshold");

static int param_set_low_threshold(const char *val, const struct kernel_param *kp)
{
	unsigned int threshold;
	int ret;

	ret = kstrtouint(val, 0, &threshold);
	if (ret)
		return ret;

	if (threshold >= sm_core_ctx.high_threshold) {
		pr_err("steal_monitor: low_threshold (%u) must be less than high_threshold (%u)\n",
		       threshold, sm_core_ctx.high_threshold);
		return -EINVAL;
	}

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops low_threshold_ops = {
	.set = param_set_low_threshold,
	.get = param_get_uint,
};

module_param_cb(low_threshold, &low_threshold_ops, &sm_core_ctx.low_threshold, 0444);
MODULE_PARM_DESC(low_threshold,
		 "Low steal threshold. default: 200 i.e 2%. Must be < high_threshold");

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
