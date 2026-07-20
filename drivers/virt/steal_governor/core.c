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

/*
 * Returns steal time of the full system.
 * Compute collective steal time across all possible CPUs.
 */
static u64 get_system_steal_time(void)
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
static unsigned int get_num_cpus_steal_ratio(void)
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
static void decrease_preferred_cpus(void)
{
	int tmp_cpu, first_hk_cpu, last_cpu;
	const struct cpumask *first_hk_core;
	int target_cpu = nr_cpu_ids;

	guard(cpus_read_lock)();
	first_hk_cpu = cpumask_first_and(housekeeping_cpumask(HK_TYPE_KERNEL_NOISE),
					 cpu_preferred_mask);
	if (first_hk_cpu >= nr_cpu_ids)
		return;

	last_cpu = cpumask_last(cpu_preferred_mask);

	if (last_cpu >= nr_cpu_ids)
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
static void increase_preferred_cpus(void)
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

static void compute_preferred_cpus_work(struct work_struct *work)
{
	u64 curr_steal, delta_steal, delta_ns, steal_ratio;
	ktime_t now;

	now = ktime_get();
	delta_ns = ktime_to_ns(ktime_sub(now, sg_core_ctx.time));

	if (unlikely(delta_ns < NSEC_PER_MSEC)) {
		pr_err_ratelimited("steal_governor: work scheduled too soon delta_ns: %llu\n",
				   delta_ns);
		goto requeue_work;
	}

	curr_steal = get_system_steal_time();
	delta_steal = curr_steal > sg_core_ctx.steal ?
		      curr_steal - sg_core_ctx.steal : 0;

	/* Update for next calculation */
	sg_core_ctx.steal = curr_steal;
	sg_core_ctx.time = now;

	/*
	 * steal_ratio = (delta_steal * 100*100)/(delta_ns * num_cpus())
	 * To avoid possible overflow, divide the denominator early.
	 * Note minimum interval is 100ms.
	 */
	delta_ns = max_t(u64, div_u64(delta_ns * get_num_cpus_steal_ratio(),
				      100 * 100), 1);
	steal_ratio = div64_u64(delta_steal, delta_ns);

	if (steal_ratio > sg_core_ctx.high_threshold)
		decrease_preferred_cpus();
	if (steal_ratio <= sg_core_ctx.low_threshold)
		increase_preferred_cpus();

	/* maintain design constructs always */
	if (cpumask_empty(cpu_preferred_mask)) {
		pr_err("empty preferred mask. stop steal governor\n");
		restore_preferred_to_active();
		return;
	}

	if (!cpumask_subset(cpu_preferred_mask, cpu_active_mask)) {
		pr_err("preferred: %*pbl is not subset of active: %*pbl, stop steal governor\n",
		       cpumask_pr_args(cpu_preferred_mask),
		       cpumask_pr_args(cpu_active_mask));
		restore_preferred_to_active();
		return;
	}

requeue_work:
	/* Trigger for next sampling */
	schedule_delayed_work(&sg_core_ctx.work,
			      msecs_to_jiffies(sg_core_ctx.interval_ms));
}

static int __init steal_governor_init(void)
{
	if (sg_core_ctx.low_threshold >= sg_core_ctx.high_threshold) {
		pr_err("steal_governor: low_threshold (%u) must be less than high_threshold (%u)\n",
		       sg_core_ctx.low_threshold, sg_core_ctx.high_threshold);
		return -EINVAL;
	}

	pr_info("steal_governor is enabled. interval: %ums, high_threshold: %u, low_threshold: %u\n",
		sg_core_ctx.interval_ms, sg_core_ctx.high_threshold, sg_core_ctx.low_threshold);

	INIT_DELAYED_WORK(&sg_core_ctx.work, compute_preferred_cpus_work);
	sg_core_ctx.steal = get_system_steal_time();
	sg_core_ctx.time = ktime_get();

	schedule_delayed_work(&sg_core_ctx.work,
			      msecs_to_jiffies(sg_core_ctx.interval_ms));

	return 0;
}

static void __exit steal_governor_exit(void)
{
	disable_delayed_work_sync(&sg_core_ctx.work);
	restore_preferred_to_active();
	pr_info("steal_governor is disabled\n");
}

module_init(steal_governor_init);
module_exit(steal_governor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Virtualization Steal Time Governor");
