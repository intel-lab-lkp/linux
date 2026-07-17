// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/err.h>
#include <linux/workqueue.h>

#include "kwatch.h"

static DECLARE_WAIT_QUEUE_HEAD(kwatch_anchor_wq);
static struct task_struct *kwatch_anchor_tsk;
static bool kwatch_anchor_expired;

bool kwatch_anchor_has_expired(void)
{
	return READ_ONCE(kwatch_anchor_expired);
}

void kwatch_anchor_clear_expired(void)
{
	WRITE_ONCE(kwatch_anchor_expired, false);
}

static void kwatch_auto_stop_handler(struct work_struct *work)
{
	kwatch_auto_stop();
}

static DECLARE_WORK(kwatch_auto_stop_work, kwatch_auto_stop_handler);

noinline void kwatch_global_anchor(unsigned long duration_sec)
{
	/* TASK_IDLE: a long timed sleep must not inflate loadavg or trip the
	 * hung-task detector the way TASK_UNINTERRUPTIBLE would.
	 */
	wait_event_idle_timeout(kwatch_anchor_wq, kthread_should_stop(),
				duration_sec * HZ);
}

static int kwatch_anchor_thread_fn(void *data)
{
	unsigned long duration = (unsigned long)data;

	kwatch_global_anchor(duration);

	if (!kthread_should_stop()) {
		/* mark before scheduling; cleared under the control mutex */
		WRITE_ONCE(kwatch_anchor_expired, true);
		schedule_work(&kwatch_auto_stop_work);
	}

	while (!kthread_should_stop())
		schedule_timeout_idle(HZ);

	return 0;
}

int kwatch_anchor_start(u16 duration)
{
	kwatch_anchor_tsk = kthread_run(kwatch_anchor_thread_fn,
					(void *)(unsigned long)duration,
					"kwatch_anchor");
	if (IS_ERR(kwatch_anchor_tsk)) {
		int ret = PTR_ERR(kwatch_anchor_tsk);

		kwatch_anchor_tsk = NULL;
		return ret;
	}
	return 0;
}

void kwatch_anchor_stop(void)
{
	if (kwatch_anchor_tsk) {
		kthread_stop(kwatch_anchor_tsk);
		kwatch_anchor_tsk = NULL;
	}
}

void kwatch_anchor_cancel_work(void)
{
	cancel_work_sync(&kwatch_auto_stop_work);
}
