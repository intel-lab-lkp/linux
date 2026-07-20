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

static struct steal_governor sg_core_ctx = {
	.interval_ms	=	1000,	/* 1 second */
	.high_threshold =	500,	/* 5% */
	.low_threshold	=	200,	/* 2% */
};

static void restore_preferred_to_active(void)
{
	int cpu;

	guard(cpus_read_lock)();
	for_each_cpu(cpu, cpu_active_mask)
		set_cpu_preferred(cpu, true);
}

static int param_set_interval_ms(const char *val, const struct kernel_param *kp)
{
	unsigned int interval;
	int ret;

	ret = kstrtouint(val, 0, &interval);
	if (ret)
		return ret;

	if (interval < 100 || interval > 100000) {
		pr_err("steal_governor: interval_ms must be between 100 and 100000\n");
		return -EINVAL;
	}

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops interval_ms_ops = {
	.set = param_set_interval_ms,
	.get = param_get_uint,
};

module_param_cb(interval_ms, &interval_ms_ops, &sg_core_ctx.interval_ms, 0444);
MODULE_PARM_DESC(interval_ms,
		 "Sampling frequency in milliseconds. default: 1000");

static int param_set_high_threshold(const char *val, const struct kernel_param *kp)
{
	unsigned int threshold;
	int ret;

	ret = kstrtouint(val, 0, &threshold);
	if (ret)
		return ret;

	if (threshold >= 100 * 100) {
		pr_err("steal_governor: high_threshold (%u) can't be more than 99.99%%\n",
		       threshold);
		return -EINVAL;
	}

	return param_set_uint(val, kp);
}

static const struct kernel_param_ops high_threshold_ops = {
	.set = param_set_high_threshold,
	.get = param_get_uint,
};

module_param_cb(high_threshold, &high_threshold_ops, &sg_core_ctx.high_threshold, 0444);
MODULE_PARM_DESC(high_threshold,
		 "High steal threshold. default: 500 i.e 5%. Must be > low_threshold");

module_param_named(low_threshold, sg_core_ctx.low_threshold, uint, 0444);
MODULE_PARM_DESC(low_threshold,
		 "Low steal threshold. default: 200 i.e 2%. Must be < high_threshold");

static int __init steal_governor_init(void)
{
	if (sg_core_ctx.low_threshold >= sg_core_ctx.high_threshold) {
		pr_err("steal_governor: low_threshold (%u) must be less than high_threshold (%u)\n",
		       sg_core_ctx.low_threshold, sg_core_ctx.high_threshold);
		return -EINVAL;
	}

	pr_info("steal_governor is enabled. interval: %ums, high_threshold: %u, low_threshold: %u\n",
		sg_core_ctx.interval_ms, sg_core_ctx.high_threshold, sg_core_ctx.low_threshold);

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
