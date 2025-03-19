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


static struct task_struct *kmmscand_thread __read_mostly;
static DEFINE_MUTEX(kmmscand_mutex);

/* How long to pause between two scan and migration cycle */
static unsigned int kmmscand_scan_sleep_ms __read_mostly = 16;

/* Max number of mms to scan in one scan and migration cycle */
#define KMMSCAND_MMS_TO_SCAN	(4 * 1024UL)
static unsigned long kmmscand_mms_to_scan __read_mostly = KMMSCAND_MMS_TO_SCAN;

bool kmmscand_scan_enabled = true;
static bool need_wakeup;

static unsigned long kmmscand_sleep_expire;

static DECLARE_WAIT_QUEUE_HEAD(kmmscand_wait);

struct kmmscand_scan {
	struct list_head mm_head;
};

struct kmmscand_scan kmmscand_scan = {
	.mm_head = LIST_HEAD_INIT(kmmscand_scan.mm_head),
};

static int kmmscand_has_work(void)
{
	return !list_empty(&kmmscand_scan.mm_head);
}

static bool kmmscand_should_wakeup(void)
{
	bool wakeup =  kthread_should_stop() || need_wakeup ||
	       time_after_eq(jiffies, kmmscand_sleep_expire);
	if (need_wakeup)
		need_wakeup = false;

	return wakeup;
}

static void kmmscand_wait_work(void)
{
	const unsigned long scan_sleep_jiffies =
		msecs_to_jiffies(kmmscand_scan_sleep_ms);

	if (!scan_sleep_jiffies)
		return;

	kmmscand_sleep_expire = jiffies + scan_sleep_jiffies;
	wait_event_timeout(kmmscand_wait,
			kmmscand_should_wakeup(),
			scan_sleep_jiffies);
	return;
}

static unsigned long kmmscand_scan_mm_slot(void)
{
	/* placeholder for scanning */
	msleep(100);
	return 0;
}

static void kmmscand_do_scan(void)
{
	unsigned long iter = 0, mms_to_scan;

	mms_to_scan = READ_ONCE(kmmscand_mms_to_scan);

	while (true) {
		cond_resched();

		if (unlikely(kthread_should_stop()) ||
			!READ_ONCE(kmmscand_scan_enabled))
			break;

		if (kmmscand_has_work())
			kmmscand_scan_mm_slot();

		iter++;
		if (iter >= mms_to_scan)
			break;
	}
}

static int kmmscand(void *none)
{
	for (;;) {
		if (unlikely(kthread_should_stop()))
			break;

		kmmscand_do_scan();

		while (!READ_ONCE(kmmscand_scan_enabled)) {
			cpu_relax();
			kmmscand_wait_work();
		}

		kmmscand_wait_work();
	}
	return 0;
}

static int start_kmmscand(void)
{
	int err = 0;

	guard(mutex)(&kmmscand_mutex);

	/* Some one already succeeded in starting daemon */
	if (kmmscand_thread)
		goto end;

	kmmscand_thread = kthread_run(kmmscand, NULL, "kmmscand");
	if (IS_ERR(kmmscand_thread)) {
		pr_err("kmmscand: kthread_run(kmmscand) failed\n");
		err = PTR_ERR(kmmscand_thread);
		kmmscand_thread = NULL;
		goto end;
	} else {
		pr_info("kmmscand: Successfully started kmmscand");
	}

	if (!list_empty(&kmmscand_scan.mm_head))
		wake_up_interruptible(&kmmscand_wait);

end:
	return err;
}

static int stop_kmmscand(void)
{
	int err = 0;

	guard(mutex)(&kmmscand_mutex);

	if (kmmscand_thread) {
		kthread_stop(kmmscand_thread);
		kmmscand_thread = NULL;
	}

	return err;
}

static int __init kmmscand_init(void)
{
	int err;

	err = start_kmmscand();
	if (err)
		goto err_kmmscand;

	return 0;

err_kmmscand:
	stop_kmmscand();

	return err;
}
subsys_initcall(kmmscand_init);
