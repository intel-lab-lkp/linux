// SPDX-License-Identifier: GPL-2.0-only
/*
 * Workingset report kernel aging thread
 *
 * Performs aging on behalf of memcgs with their configured refresh interval.
 * While a userspace program can periodically read the page age breakdown
 * per-memcg and trigger aging, the kernel performing aging is less overhead,
 * more consistent, and more reliable for the use case where every memcg should
 * be aged according to their refresh interval.
 */
#define pr_fmt(fmt) "workingset report aging: " fmt

#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/swap.h>
#include <linux/wait.h>
#include <linux/mmzone.h>
#include <linux/workingset_report.h>

static DECLARE_WAIT_QUEUE_HEAD(aging_wait);
static bool refresh_pending;

static bool do_aging_node(int nid, unsigned long *next_wake_time)
{
	struct mem_cgroup *memcg;
	bool should_wait = true;
	struct pglist_data *pgdat = NODE_DATA(nid);

	memcg = mem_cgroup_iter(NULL, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
		struct wsr_state *wsr = &lruvec->wsr;
		unsigned long refresh_time;

		/* use returned time to decide when to wake up next */
		if (wsr_refresh_report(wsr, memcg, pgdat, &refresh_time)) {
			if (should_wait) {
				should_wait = false;
				*next_wake_time = refresh_time;
			} else if (time_before(refresh_time, *next_wake_time)) {
				*next_wake_time = refresh_time;
			}
		}

		cond_resched();
	} while ((memcg = mem_cgroup_iter(NULL, memcg, NULL)));

	return should_wait;
}

static int do_aging(void *unused)
{
	while (!kthread_should_stop()) {
		int nid;
		long timeout_ticks;
		unsigned long next_wake_time;
		bool should_wait = true;

		WRITE_ONCE(refresh_pending, false);
		for_each_node_state(nid, N_MEMORY) {
			unsigned long node_next_wake_time;

			if (do_aging_node(nid, &node_next_wake_time))
				continue;
			if (should_wait) {
				should_wait = false;
				next_wake_time = node_next_wake_time;
			} else if (time_before(node_next_wake_time,
					       next_wake_time)) {
				next_wake_time = node_next_wake_time;
			}
		}

		if (should_wait) {
			wait_event_interruptible(aging_wait, refresh_pending);
			continue;
		}

		/* sleep until next aging */
		timeout_ticks = next_wake_time - jiffies;
		if (timeout_ticks > 0 &&
		    timeout_ticks != MAX_SCHEDULE_TIMEOUT) {
			schedule_timeout_idle(timeout_ticks);
			continue;
		}
	}
	return 0;
}

/* Invoked when refresh_interval shortens or changes to a non-zero value. */
void wsr_wakeup_aging_thread(void)
{
	WRITE_ONCE(refresh_pending, true);
	wake_up_interruptible(&aging_wait);
}

static struct task_struct *aging_thread;

static int aging_init(void)
{
	struct task_struct *task;

	task = kthread_run(do_aging, NULL, "kagingd");

	if (IS_ERR(task)) {
		pr_err("Failed to create aging kthread\n");
		return PTR_ERR(task);
	}

	aging_thread = task;
	pr_info("module loaded\n");
	return 0;
}

static void aging_exit(void)
{
	kthread_stop(aging_thread);
	aging_thread = NULL;
	pr_info("module unloaded\n");
}

module_init(aging_init);
module_exit(aging_exit);
