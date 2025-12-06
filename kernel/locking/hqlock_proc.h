/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_HQ_SPINLOCK_SLOWPATH
#error "Do not include this file!"
#endif

#include <linux/sysctl.h>

/*
 * Local handoffs threshold to maintain global fairness,
 * perform remote handoff if it's reached
 */
unsigned long hqlock_fairness_threshold = 1000;

/*
 * Minimal amount of handoffs in LOCK_MODE_QSPINLOCK
 * to enable NUMA-awareness
 */
unsigned long hqlock_general_handoffs_turn_numa = 50;

static unsigned long long_zero;
static unsigned long long_max = LONG_MAX;

static const struct ctl_table hqlock_settings[] = {
	{
		.procname		= "hqlock_fairness_threshold",
		.data			= &hqlock_fairness_threshold,
		.maxlen			= sizeof(hqlock_fairness_threshold),
		.mode			= 0644,
		.proc_handler	= proc_doulongvec_minmax
	},
	{
		.procname		= "hqlock_general_handoffs_turn_numa",
		.data			= &hqlock_general_handoffs_turn_numa,
		.maxlen			= sizeof(hqlock_general_handoffs_turn_numa),
		.mode			= 0644,
		.proc_handler	= proc_doulongvec_minmax,
		.extra1		= &long_zero,
		.extra2		= &long_max,
	},
};
static int __init init_numa_spinlock_sysctl(void)
{
	if (!register_sysctl("kernel", hqlock_settings))
		return -EINVAL;
	return 0;
}
core_initcall(init_numa_spinlock_sysctl);


#ifdef CONFIG_HQSPINLOCKS_DEBUG
static int max_buckets_in_use;
static int max_general_handoffs;
static atomic_t cur_buckets_in_use = ATOMIC_INIT(0);

static int print_hqlock_stats(struct seq_file *file, void *v)
{
	seq_printf(file, "Max dynamic metada in use after previous print: %d\n",
		   READ_ONCE(max_buckets_in_use));
	WRITE_ONCE(max_buckets_in_use, 0);

	seq_printf(file, "Max MCS handoffs after previous print: %d\n",
		   READ_ONCE(max_general_handoffs));
	WRITE_ONCE(max_general_handoffs, 0);

	return 0;
}


static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, print_hqlock_stats, NULL);
}

static const struct proc_ops stats_ops = {
	.proc_open  = stats_open,
	.proc_read  = seq_read,
	.proc_lseek = seq_lseek,
};

static int __init stats_init(void)
{
	proc_create("hqlock_stats", 0444, NULL, &stats_ops);
	return 0;
}

core_initcall(stats_init);

#endif // HQSPINLOCKS_DEBUG
