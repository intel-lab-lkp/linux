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

enum sm_direction {
	SM_DIR_INCREASE = -1,
	SM_DIR_NONE	=  0,
	SM_DIR_DECREASE	=  1,
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

static void compute_preferred_cpus_work(struct work_struct *work)
{
	u64 curr_steal, delta_steal, delta_ns, steal_ratio;
	ktime_t now;

	now = ktime_get();
	delta_ns = ktime_to_ns(ktime_sub(now, sm_core_ctx.prev_time));

	if (unlikely(delta_ns < NSEC_PER_MSEC)) {
		pr_err_ratelimited("steal_monitor: work scheduled too soon delta_ns: %llu\n",
				   delta_ns);
		goto requeue_work;
	}

	curr_steal = get_system_steal_time();
	delta_steal = curr_steal > sm_core_ctx.prev_steal ?
		      curr_steal - sm_core_ctx.prev_steal : 0;

	/* Update for next calculation */
	sm_core_ctx.prev_steal = curr_steal;
	sm_core_ctx.prev_time = now;

	/*
	 * steal_ratio = (delta_steal * 100*100)/(delta_ns * num_cpus())
	 * To avoid possible overflow, divide the denominator early.
	 * Note minimum interval is 10ms.
	 */
	delta_ns = div_u64(delta_ns * get_num_cpus_steal_ratio(), 100 * 100);
	steal_ratio = div64_u64(delta_steal, delta_ns);

	if (sm_core_ctx.prev_direction == SM_DIR_DECREASE &&
	    steal_ratio > sm_core_ctx.high_threshold)
		decrease_preferred_cpus(&sm_core_ctx);
	if (sm_core_ctx.prev_direction == SM_DIR_INCREASE &&
	    steal_ratio <= sm_core_ctx.low_threshold)
		increase_preferred_cpus(&sm_core_ctx);

	/*
	 * mark the direction. Increasing the gap between hi and lo_threshold
	 * helps to avoid ping-pongs.
	 */
	if (steal_ratio > sm_core_ctx.high_threshold)
		sm_core_ctx.prev_direction = SM_DIR_DECREASE;
	else if (steal_ratio <= sm_core_ctx.low_threshold)
		sm_core_ctx.prev_direction = SM_DIR_INCREASE;
	else
		sm_core_ctx.prev_direction = SM_DIR_NONE;

requeue_work:
	/* maintain design constructs always */
	WARN_ON_ONCE(cpumask_empty(cpu_preferred_mask));
	WARN_ON_ONCE(!cpumask_subset(cpu_preferred_mask, cpu_active_mask));

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
	cancel_delayed_work_sync(&sm_core_ctx.work);
	guard(cpus_read_lock)();
	cpumask_copy(&__cpu_preferred_mask, cpu_active_mask);

	pr_info("steal_monitor is disabled\n");
}

module_init(steal_monitor_init);
module_exit(steal_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Monitor");
