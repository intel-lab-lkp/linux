// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc
 *
 * Author: Dhaval Giani (AMD) <dhaval@gianis.ca>
 *
 * Basic functional tests for EEVDF - Invariants
 *
 * Use the debugfs triggers to run them
 *
 */

#include <linux/debugfs.h>
#include <linux/sched.h>

#include "sched.h"

#ifdef CONFIG_SCHED_EEVDF_TESTING

/*
 * Test parameters
 */
bool eevdf_positive_lag_test;
u8 eevdf_positive_lag_count = 10;

static int test_total_zero_lag(void *);
static void launch_test_zero_lag(void);

static int eevdf_zero_lag_show(struct seq_file *m, void *v)
{
	return 0;
}

static int eevdf_zero_lag_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, eevdf_zero_lag_show, NULL);
}

static ssize_t eevdf_zero_lag_write(struct file *filp, const char __user *ubuf,
				   size_t cnt, loff_t *ppos)
{
	launch_test_zero_lag();
	return 1;

}

static const struct file_operations eevdf_zero_lag_fops = {
	.open		= eevdf_zero_lag_open,
	.write		= eevdf_zero_lag_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct dentry *debugfs_eevdf_testing;
void debugfs_eevdf_testing_init(struct dentry *debugfs_sched)
{
	debugfs_eevdf_testing = debugfs_create_dir("eevdf-testing", debugfs_sched);

	debugfs_create_bool("eevdf_positive_lag_test", 0700,
				debugfs_eevdf_testing, &eevdf_positive_lag_test);
	debugfs_create_u8("eevdf_positive_lag_test_count", 0600,
				debugfs_eevdf_testing, &eevdf_positive_lag_count);
	debugfs_create_file("eevdf_zero_lag_test", 0700, debugfs_eevdf_testing,
				NULL, &eevdf_zero_lag_fops);

}

void test_eevdf_positive_lag(struct cfs_rq *cfs, struct sched_entity *se)
{
	static int eevdf_positive_lag_test_counter;
	u64 eevdf_average_vruntime;

	if (!eevdf_positive_lag_test)
		return;

	if (!se || !cfs)
		return;

	eevdf_average_vruntime = avg_vruntime(cfs);
	eevdf_positive_lag_test_counter++;

	if (se->vruntime > eevdf_average_vruntime) {
		trace_printk("FAIL: Lemma 1 failed - selected task has negative lag\n");
		eevdf_positive_lag_test = 0;
		eevdf_positive_lag_test_counter = 0;
		return;
	}

	if (eevdf_positive_lag_test_counter > eevdf_positive_lag_count) {
		eevdf_positive_lag_test = 0;
		eevdf_positive_lag_test_counter = 0;
		trace_printk("PASS: At least %u selected tasks had positive lag\n",
							eevdf_positive_lag_count);
	}
}

/*
 * we do, what we need to do
 */
#define __node_2_se(node) \
	rb_entry((node), struct sched_entity, run_node)

static bool test_eevdf_cfs_rq_zero_lag(struct cfs_rq *cfs, struct list_head *tg_se)
{
	u64 cfs_avg_vruntime;
	u64 calculated_avg_vruntime;

	u64 total_vruntime = 0;
	u64 nr_tasks = 0;

	struct sched_entity *se;
	struct rb_node *node;
	struct rb_root *root;

	cfs_avg_vruntime = avg_vruntime(cfs);

	/*
	 * Walk through the rb tree -> look at the se->vruntime value and add it
	 */

	total_vruntime = 0;
	nr_tasks = 0;

	root = &cfs->tasks_timeline.rb_root;

	for (node = rb_first(root); node; node = rb_next(node)) {
		se = __node_2_se(node);
		WARN_ON_ONCE(__builtin_add_overflow(total_vruntime,
					se->vruntime, &total_vruntime));
		/*
		 * if it is a task group, add to a list to look at later
		 */
		if (!entity_is_task(se))
			list_add_tail(&se->tg_entry, tg_se);
		nr_tasks++;
	}

	if (cfs->curr) {
		WARN_ON_ONCE(__builtin_add_overflow(total_vruntime,
					cfs->curr->vruntime, &total_vruntime));
		nr_tasks++;
	}

	/* If there are no tasks, there is no lag :-) */
	if (!nr_tasks)
		return true;

	calculated_avg_vruntime = total_vruntime / nr_tasks;

	return (calculated_avg_vruntime == cfs_avg_vruntime);

}

/*
 * Call with rq lock held
 *
 * return false on failure
 */
static bool test_eevdf_zero_lag(struct cfs_rq *cfs)
{
	struct list_head tg_se = LIST_HEAD_INIT(tg_se);
	struct list_head *se_entry;

	/*
	 * The base CFS runqueue will always have sched entities queued.
	 * Test it, and start populating the tg_se list.
	 *
	 * If it fails, short circuit and return fail.
	 */

	if (!test_eevdf_cfs_rq_zero_lag(cfs, &tg_se))
		return false;

	/*
	 * We made it here, let's walk through the list. Since it is
	 * setup as a queue, as we continue calling the rq test, it
	 * will add new task_groups to the list. Once drained, if we
	 * haven't failed, we will return true.
	 */

	list_for_each(se_entry, &tg_se) {
		struct sched_entity *se = list_entry(se_entry, struct sched_entity, tg_entry);

		if (!test_eevdf_cfs_rq_zero_lag(group_cfs_rq(se), &tg_se))
			return false;
	}

	/*
	 * WOOT! We succeeded!
	 */
	return true;

}

/*
 * The average vruntime of the entire cfs_rq should be equal
 * to the avg_vruntime(cfs_rq)
 */
static int test_total_zero_lag(void *data)
{
	int cpu;
	struct rq *rq;
	struct cfs_rq *cfs;
	bool success = false;

	for_each_online_cpu(cpu) {

		rq = cpu_rq(cpu);
		guard(rq_lock_irq)(rq);

		cfs = &rq->cfs;

		success = test_eevdf_zero_lag(cfs);

		if (!success)
			break;
	}
	if (!success) {
		trace_printk("FAILED: tracked average vruntime doesn't match calculated average vruntime\n");
		return -1;
	}
	trace_printk("PASS: Tracked average runtime matches calculated average vruntime\n");
	return 0;
}

static void launch_test_zero_lag(void)
{
	struct task_struct *kt;

	kt = kthread_create(&test_total_zero_lag, NULL, "eevdf-tester-%d",
					smp_processor_id());
	if (!kt) {
		trace_printk("Failed to launch kthread\n");
		return;
	}

	wake_up_process(kt);
}

#endif /* CONFIG_SCHED_EEVDF_TESTING */
