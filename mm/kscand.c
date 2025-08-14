// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/cleanup.h>

#include <asm/pgalloc.h>
#include "internal.h"

static struct task_struct *kscand_thread __read_mostly;
static DEFINE_MUTEX(kscand_mutex);

/* How long to pause between two scan cycles */
static unsigned int kscand_scan_sleep_ms __read_mostly = 20;

/* Max number of mms to scan in one scan cycle */
#define KSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kscand_mms_to_scan __read_mostly = KSCAND_MMS_TO_SCAN;

bool kscand_scan_enabled = true;
static bool need_wakeup;

static unsigned long kscand_sleep_expire;

static DECLARE_WAIT_QUEUE_HEAD(kscand_wait);

/* Data structure to keep track of current mm under scan */
struct kscand_scan {
	struct list_head mm_head;
};

struct kscand_scan kscand_scan = {
	.mm_head = LIST_HEAD_INIT(kscand_scan.mm_head),
};

static inline int kscand_has_work(void)
{
	return !list_empty(&kscand_scan.mm_head);
}

static inline bool kscand_should_wakeup(void)
{
	bool wakeup = kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kscand_sleep_expire);

	need_wakeup = false;

	return wakeup;
}

static void kscand_wait_work(void)
{
	const unsigned long scan_sleep_jiffies =
		msecs_to_jiffies(kscand_scan_sleep_ms);

	if (!scan_sleep_jiffies)
		return;

	kscand_sleep_expire = jiffies + scan_sleep_jiffies;

	/* Allows kthread to pause scanning */
	wait_event_timeout(kscand_wait, kscand_should_wakeup(),
			scan_sleep_jiffies);
}
static void kscand_do_scan(void)
{
	unsigned long iter = 0, mms_to_scan;

	mms_to_scan = READ_ONCE(kscand_mms_to_scan);

	while (true) {
		if (unlikely(kthread_should_stop()) ||
			!READ_ONCE(kscand_scan_enabled))
			break;

		if (kscand_has_work())
			msleep(100);

		iter++;

		if (iter >= mms_to_scan)
			break;
		cond_resched();
	}
}

static int kscand(void *none)
{
	while (true) {
		if (unlikely(kthread_should_stop()))
			break;

		while (!READ_ONCE(kscand_scan_enabled)) {
			cpu_relax();
			kscand_wait_work();
		}

		kscand_do_scan();

		kscand_wait_work();
	}
	return 0;
}

static int start_kscand(void)
{
	struct task_struct *kthread;

	guard(mutex)(&kscand_mutex);

	if (kscand_thread)
		return 0;

	kthread = kthread_run(kscand, NULL, "kscand");
	if (IS_ERR(kscand_thread)) {
		pr_err("kscand: kthread_run(kscand) failed\n");
		return PTR_ERR(kthread);
	}

	kscand_thread = kthread;
	pr_info("kscand: Successfully started kscand");

	if (!list_empty(&kscand_scan.mm_head))
		wake_up_interruptible(&kscand_wait);

	return 0;
}

static int stop_kscand(void)
{
	guard(mutex)(&kscand_mutex);

	if (kscand_thread) {
		kthread_stop(kscand_thread);
		kscand_thread = NULL;
	}

	return 0;
}

static int __init kscand_init(void)
{
	int err;

	err = start_kscand();
	if (err)
		goto err_kscand;

	return 0;

err_kscand:
	stop_kscand();

	return err;
}
subsys_initcall(kscand_init);
