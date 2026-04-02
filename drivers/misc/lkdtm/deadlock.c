// SPDX-License-Identifier: GPL-2.0
/*
 * This is for all the tests related to deadlock.
 */
#include "lkdtm.h"
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/pagemap.h>

static struct folio *folio_common;

/*
 * Triggering a simple AA deadlock on a folio, Attempting to acquire the same
 * folio twice in the same execution context, resulting in a self-deadlock.
 */
static void lkdtm_FOLIO_LOCK_AA(void)
{
	folio_common = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio_common) {
		pr_err("folio_alloc() failed.\n");
		return;
	}

	folio_lock(folio_common);
	folio_lock(folio_common);

	/* Unreachable */
	folio_unlock(folio_common);

	folio_put(folio_common);
}

/*
 * Attempting the 'AB' order for ABBA deadlock
 */
static int lkdtm_folio_ab_kthread(void *folio_b)
{
	while (true) {
		folio_lock(folio_common);
		folio_lock((struct folio *)folio_b);
		folio_unlock((struct folio *)folio_b);
		folio_unlock(folio_common);
	}

	return 0;
}

/*
 * Attempting the 'BA' order for ABBA deadlock
 */
static int lkdtm_folio_ba_kthread(void *folio_b)
{
	while (true) {
		folio_lock((struct folio *)folio_b);
		folio_lock(folio_common);
		folio_unlock(folio_common);
		folio_unlock((struct folio *)folio_b);
	}

	return 0;
}

/*
 * Spawning kthreads that attempt to acquire Waiter A and Waiter B in reverse
 * order. Leading to a state where Thread A holds Waiter A and waits for
 * Waiter B, while Thread B holds Waiter B and waits for Waiter A.
 */
static void lkdtm_FOLIO_LOCK_ABBA(void)
{
	struct folio *folio_b;
	struct task_struct *t0, *t1;

	folio_common = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	folio_b = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	if (!folio_common || !folio_b) {
		pr_err("folio_alloc() failed.\n");
		return;
	}

	t0 = kthread_run(lkdtm_folio_ab_kthread, folio_b, "lkdtm_folio_a");
	t1 = kthread_run(lkdtm_folio_ba_kthread, folio_b, "lkdtm_folio_b");

	if (IS_ERR(t0) || IS_ERR(t1))
		pr_err("failed to start kthread.\n");

	folio_put(folio_common);
	folio_put(folio_b);
}

DEFINE_MUTEX(mutex_b);

/* Attempting 'folio_lock() A then Mutex B' order */
static int lkdtm_folio_mutex_kthread(void *)
{
	while (true) {
		folio_lock(folio_common);
		mutex_lock(&mutex_b);
		mutex_unlock(&mutex_b);
		folio_unlock(folio_common);
	}

	return 0;
}

/* Attempting 'Mutex B then folio_lock() A' order */
static int lkdtm_mutex_folio_kthread(void *)
{
	while (true) {
		mutex_lock(&mutex_b);
		folio_lock(folio_common);
		folio_unlock(folio_common);
		mutex_unlock(&mutex_b);
	}

	return 0;
}

/* Triggering ABBA deadlock between folio_lock() and mutex. */
static void lkdtm_FOLIO_MUTEX_LOCK_ABBA(void)
{
	struct task_struct *t0, *t1;

	folio_common = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);

	t0 = kthread_run(lkdtm_folio_mutex_kthread, NULL, "lkdtm_folio_mutex");
	t1 = kthread_run(lkdtm_mutex_folio_kthread, NULL, "lkdtm_mutex_folio");

	if (IS_ERR(t0) || IS_ERR(t1))
		pr_err("failed to start kthreads\n");

	folio_put(folio_common);
}

/*
 * Deferred AB-BA Deadlock Scenario
 *
 * Deferring a lock acquisition from an atomic wake-up callback to a
 * sleepable workqueue context.
 *
 * ----------------------------------------------------------------------
 * 'lkdtm_waiter' kthread          Waker               kworker thread
 *  [Sleepable Context]        (LKDTM Trigger)       [Sleepable Context]
 *           |                       |                      |
 *    1. folio_lock(folio_common)    |                      |
 *       [Holds Folio]               |                      |
 *           |                       |                      |
 *    [Waits for Wakeup] <---  2. wake_up(&wq_deadlock)     |
 *           |                       |                      |
 *           |              3. wake_for_deferred_work()     |
 *           |                  [Inside wq_deadlock->lock]  |
 *           |                       |                      |
 *           |              4. schedule_work() --->         |
 *           |                       |          5. deferred_deadlocking()
 *           |                       |                      |
 *           |                       |          6. folio_lock(folio_common)
 *           |                       |               [Waits for Folio]
 *           |                       |                      |
 * ----------------------------------------------------------------------
 */
static DECLARE_WAIT_QUEUE_HEAD(wq_deadlock);
static DECLARE_COMPLETION(waiter_ready);
static struct folio *folio_common;
static struct work_struct deadlock_work;

/**
 * deferred_deadlocking - The deferred task executed by a kworker thread.
 * @work: The work structure associated with this task.
 *
 * Since this runs in a kworker thread, it is a safe sleepable context.
 * Attempting to acquire the folio_lock here will not cause an atomic
 * scheduling violation, but it will create a logical deadlock and a
 * circular dependency.
 */
static void deferred_deadlocking(struct work_struct *work)
{
	pr_info("[Worker Context] Attempting to acquire folio_lock...\n");

	/*
	 * DEADLOCK POINT:
	 * The kworker blocks here indefinitely because the lkdtm_waiter
	 * thread holds the PG_locked bit of folio_common.
	 */
	folio_lock(folio_common);

	/* Unreachable */
	folio_unlock(folio_common);

	folio_put(folio_common);

	wake_up_all(&wq_deadlock);
}

/**
 * wake_for_deferred_work - Invoking the deferred_waker_work().
 * @wq_entry: The wait queue entry being woken up.
 * @mode: Wakeup mode (e.g., TASK_NORMAL).
 * @sync: Indicates if the wakeup is synchronous.
 * @key: Event-specific key passed to wake_up().
 *
 * Return: Always 0, meaning the waiter is not woken up and
 * remains in the wait queue.
 */
static int wake_for_deferred_work(struct wait_queue_entry *wq_entry,
				  unsigned int mode, int sync, void *key)
{
	pr_emerg(
		"[Waker Context] Atomic callback triggered. Deferring work...\n");

	schedule_work(&deadlock_work);

	return 0;
}

/**
 * lkdtm_waiter_thread - The background thread holding the lock.
 *
 * It acquires the folio lock, signals readiness to the trigger process,
 * and then goes to sleep on the custom wait queue.
 *
 * Return: 0 on exit (unreachable in successful deadlock).
 */
static int lkdtm_waiter_thread(void *)
{
	struct wait_queue_entry custom_wait;

	init_waitqueue_func_entry(&custom_wait, wake_for_deferred_work);

	pr_info("[Waiter Thread] Securing the folio_lock...\n");
	folio_lock(folio_common);

	complete(&waiter_ready);

	pr_info("[Waiter Thread] Lock secured. Sleeping on wait queue.\n");

	add_wait_queue(&wq_deadlock, &custom_wait);

	/*
	 * Manual sleep logic. We sleep without a condition because we
	 * expect the deferred work to eventually wake us up.
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule();

	/* Unreachable */
	__set_current_state(TASK_RUNNING);
	remove_wait_queue(&wq_deadlock, &custom_wait);
	folio_unlock(folio_common);

	return 0;
}

/*
 * Spawns the waiter thread, and triggers the wait queue wakeup mechanism to
 * initiate the deferred deadlock.
 */
static void lkdtm_FOLIO_DEFERRED_EVENT_ABBA(void)
{
	struct task_struct *waiter_task;

	folio_common = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!folio_common) {
		pr_err("Failed to allocate folio.\n");
		return;
	}


	INIT_WORK(&deadlock_work, deferred_deadlocking);
	reinit_completion(&waiter_ready);

	waiter_task = kthread_run(lkdtm_waiter_thread, NULL, "lkdtm_waiter");
	if (IS_ERR(waiter_task)) {
		pr_err("Failed to create waiter thread.\n");
		folio_put(folio_common);
		return;
	}

	wait_for_completion(&waiter_ready);

	pr_info("[Trigger] Calling wake_up() to initiate deferred deadlock.\n");

	/*
	 * Triggers wake_for_deferred_work() in the current atomic context,
	 * which in turn schedules deferred_deadlocking().
	 */
	wake_up(&wq_deadlock);
}

static struct crashtype crashtypes[] = {
	CRASHTYPE(FOLIO_LOCK_AA),
	CRASHTYPE(FOLIO_LOCK_ABBA),
	CRASHTYPE(FOLIO_MUTEX_LOCK_ABBA),
	CRASHTYPE(FOLIO_DEFERRED_EVENT_ABBA),
};

struct crashtype_category deadlock_crashtypes = {
	.crashtypes = crashtypes,
	.len = ARRAY_SIZE(crashtypes),
};
