// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/dma-fence-chain.h>

#include "xe_deadline_mgr.h"
#include "xe_deadline_mgr_types.h"
#include "xe_exec_queue.h"
#include "xe_gt.h"
#include "xe_hw_fence.h"

#define XE_DEADLINE_WINDOW_US			3000
#define XE_DEADLINE_PRIO_BOOST_WINDOW_PERCENT	60
#define XE_DEADLINE_EXIT_DELAY_MS		100

static ktime_t __xe_deadline_mgr_freq_boost_window(void)
{
	return us_to_ktime(XE_DEADLINE_WINDOW_US);
}

static ktime_t __xe_deadline_mgr_prio_boost_window(void)
{
	u64 usec = DIV_ROUND_UP_ULL(XE_DEADLINE_WINDOW_US *
				    XE_DEADLINE_PRIO_BOOST_WINDOW_PERCENT, 100);

	return us_to_ktime(usec);
}

static ktime_t __xe_deadline_mgr_prio_boost_restart(void)
{
	return ktime_sub(__xe_deadline_mgr_freq_boost_window(),
			 __xe_deadline_mgr_prio_boost_window());
}

static bool __xe_deadline_mgr_deadline_signaled(struct xe_deadline_mgr *mgr)
{
	struct xe_hw_fence *hw_fence;

	lockdep_assert_held(&mgr->lock);

	hw_fence = list_first_entry_or_null(&mgr->deadlines, typeof(*hw_fence),
					    deadline.link);
	if (!hw_fence)
		return true;

	return xe_hw_fence_signaled(&hw_fence->dma);
}

static bool __xe_deadline_mgr_enter_deadline(struct xe_deadline_mgr *mgr,
					     enum xe_deadline_mgr_state state)
{
	lockdep_assert_held(&mgr->lock);

	if (XE_DEADLINE_EXIT_DELAY_MS &&
	    mgr->state != XE_DEADLINE_MGR_STATE_NO_BOOST)
		cancel_delayed_work(&mgr->exit_delay);

	if (mgr->state != state && !__xe_deadline_mgr_deadline_signaled(mgr)) {
		mgr->state = state;
		mgr->q->ops->set_deadline_state(mgr->q, state);

		return true;
	}

	return false;
}

static void __xe_deadline_mgr_exit_deadline_work(struct work_struct *work)
{
	struct xe_deadline_mgr *mgr = container_of(work, typeof(*mgr),
						   exit_delay.work);

	guard(spinlock_irqsave)(&mgr->lock);

	if (mgr->state != XE_DEADLINE_MGR_STATE_NO_BOOST) {
		mgr->state = XE_DEADLINE_MGR_STATE_NO_BOOST;
		mgr->q->ops->set_deadline_state(mgr->q, mgr->state);
	}
}

static void __xe_deadline_mgr_exit_deadline(struct xe_deadline_mgr *mgr)
{
	lockdep_assert_held(&mgr->lock);

	if (mgr->state == XE_DEADLINE_MGR_STATE_NO_BOOST)
		return;

	if (!XE_DEADLINE_EXIT_DELAY_MS) {
		mgr->state = XE_DEADLINE_MGR_STATE_NO_BOOST;
		mgr->q->ops->set_deadline_state(mgr->q, mgr->state);
		return;
	}

	if (!delayed_work_pending(&mgr->exit_delay))
		mod_delayed_work(system_percpu_wq, &mgr->exit_delay,
				 msecs_to_jiffies(XE_DEADLINE_EXIT_DELAY_MS));
}

static enum hrtimer_restart __xe_deadline_mgr_timer(struct hrtimer *t)
{
	struct xe_deadline_mgr *mgr = container_of(t, typeof(*mgr), timer);
	enum xe_deadline_mgr_state state;
	bool boosted;

	guard(spinlock_irqsave)(&mgr->lock);

	xe_assert(gt_to_xe(mgr->q->gt),
		  mgr->state != XE_DEADLINE_MGR_STATE_PRIO_BOOST ||
		  XE_DEADLINE_EXIT_DELAY_MS);

	if (mgr->state == XE_DEADLINE_MGR_STATE_NO_BOOST &&
	    XE_DEADLINE_PRIO_BOOST_WINDOW_PERCENT != 100)
		state = XE_DEADLINE_MGR_STATE_FREQ_BOOST;
	else
		state = XE_DEADLINE_MGR_STATE_PRIO_BOOST;

	boosted = __xe_deadline_mgr_enter_deadline(mgr, state);

	if (boosted && state == XE_DEADLINE_MGR_STATE_FREQ_BOOST &&
	    XE_DEADLINE_PRIO_BOOST_WINDOW_PERCENT != 0) {
		ktime_t sub = __xe_deadline_mgr_freq_boost_window();

		hrtimer_forward(t, ktime_sub(mgr->deadline, sub),
				__xe_deadline_mgr_prio_boost_restart());
		return HRTIMER_RESTART;
	}

	return HRTIMER_NORESTART;
}

/**
 * xe_deadline_mgr_init() - Deadline manager initialize
 * @mgr: Deadline manager object
 * @q: Exec queue associated with deadline
 */
void xe_deadline_mgr_init(struct xe_deadline_mgr *mgr, struct xe_exec_queue *q)
{
	mgr->q = q;
	INIT_LIST_HEAD(&mgr->deadlines);
	spin_lock_init(&mgr->lock);
	hrtimer_setup(&mgr->timer, __xe_deadline_mgr_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS);
	mgr->deadline = XE_DEADLINE_NONE;
	mgr->state = XE_DEADLINE_MGR_STATE_NO_BOOST;
	INIT_DELAYED_WORK(&mgr->exit_delay,
			  __xe_deadline_mgr_exit_deadline_work);

	/*
	 * Parallel queues are not supported because the job's fence is a
	 * dma-fence chain, which is memory-unsafe as individual hardware fences
	 * can be freed at arbitrary points in time while remaining in the
	 * manager. Multi-queue is also not supported because we need individual
	 * per-queue control of priority and frequency, which multi-queue does
	 * not have. In either case, the target use case (compositors) does not
	 * use these types of queues.
	 *
	 * Also disable the deadline logic if the feature is disabled via
	 * Kconfig or if the queue is created in a boosted state.
	 */
	if (xe_exec_queue_is_parallel(q) || xe_exec_queue_is_multi_queue(q) ||
	    !XE_DEADLINE_WINDOW_US ||
	    (q->sched_props.priority >= XE_EXEC_QUEUE_PRIORITY_HIGH &&
	     q->flags & EXEC_QUEUE_FLAG_LOW_LATENCY))
		mgr->state = XE_DEADLINE_MGR_STATE_UNSUPPORTED;
}

/**
 * xe_deadline_mgr_fini() - Deadline manager finalize
 * @mgr: Deadline manager object
 */
void xe_deadline_mgr_fini(struct xe_deadline_mgr *mgr)
{
	cancel_delayed_work_sync(&mgr->exit_delay);
	xe_assert(gt_to_xe(mgr->q->gt),
		  mgr->state == XE_DEADLINE_MGR_STATE_NO_BOOST ||
		  mgr->state == XE_DEADLINE_MGR_STATE_UNSUPPORTED);
	xe_assert(gt_to_xe(mgr->q->gt), !hrtimer_cancel(&mgr->timer));
	xe_assert(gt_to_xe(mgr->q->gt), list_empty(&mgr->deadlines));
}

static ktime_t __xe_deadline_mgr_new_deadline(struct xe_deadline_mgr *mgr)
{
	struct xe_hw_fence *hw_fence;

	lockdep_assert_held(&mgr->lock);

	hw_fence = list_first_entry_or_null(&mgr->deadlines, typeof(*hw_fence),
					    deadline.link);
	if (!hw_fence)
		return XE_DEADLINE_NONE;

	return hw_fence->deadline.time;
}

static void __xe_deadline_mgr_update_deadline(struct xe_deadline_mgr *mgr)
{
	ktime_t old_deadline = mgr->deadline, sub, deadline, now;

again:
	lockdep_assert_held(&mgr->lock);

	mgr->deadline = __xe_deadline_mgr_new_deadline(mgr);

	if (!ktime_compare(old_deadline, mgr->deadline))
		return;

	if (hrtimer_try_to_cancel(&mgr->timer) < 0) {
		/*
		 * Corner case where hrtimer is running but waiting on
		 * &mgr->lock, we need to drop the lock, cancel timer, require
		 * the lock and retry.
		 */
		spin_unlock(&mgr->lock);
		hrtimer_cancel(&mgr->timer);
		spin_lock(&mgr->lock);
		goto again;
	}

	if (mgr->deadline == XE_DEADLINE_NONE) {
		__xe_deadline_mgr_exit_deadline(mgr);
		return;
	}

	sub = __xe_deadline_mgr_freq_boost_window();
	deadline = ktime_sub(mgr->deadline, sub);
	now = ktime_get();

	if (ktime_after(now, deadline)) {
		enum xe_deadline_mgr_state state =
			XE_DEADLINE_MGR_STATE_FREQ_BOOST;

		if (mgr->state == XE_DEADLINE_MGR_STATE_PRIO_BOOST) {
			state = XE_DEADLINE_MGR_STATE_PRIO_BOOST;
		} else {
			sub = __xe_deadline_mgr_prio_boost_window();
			if (sub) {
				deadline = ktime_sub(mgr->deadline, sub);

				if (ktime_after(now, deadline))
					state = XE_DEADLINE_MGR_STATE_PRIO_BOOST;
				else
					hrtimer_start(&mgr->timer, deadline,
						      HRTIMER_MODE_ABS);
			}
		}

		__xe_deadline_mgr_enter_deadline(mgr, state);
	} else {
		__xe_deadline_mgr_exit_deadline(mgr);
		hrtimer_start(&mgr->timer, deadline,
			      HRTIMER_MODE_ABS);
	}
}

static void __xe_deadline_mgr_remove_deadline(struct xe_deadline_mgr *mgr,
					      struct xe_hw_fence *hw_fence)
{
	ktime_t old_deadline = hw_fence->deadline.time;

	lockdep_assert_held(&mgr->lock);

	hw_fence->deadline.time = XE_DEADLINE_DONE;
	if (old_deadline == XE_DEADLINE_NONE)
		return;

	list_del_init(&hw_fence->deadline.link);
	__xe_deadline_mgr_update_deadline(mgr);
}

static void __xe_deadline_mgr_add_deadline(struct xe_deadline_mgr *mgr,
					   struct xe_hw_fence *hw_fence,
					   ktime_t deadline)
{
	struct xe_hw_fence *pos;

	lockdep_assert_held(&mgr->lock);

	hw_fence->deadline.time = deadline;

	list_for_each_entry(pos, &mgr->deadlines, deadline.link) {
		if (ktime_before(hw_fence->deadline.time, pos->deadline.time)) {
			/*
			 * A bit confusing, but the below code actually inserts
			 * 'hw_fence' before 'pos' as list_add_tail effectively
			 * means insert before head.
			 */
			list_add_tail(&hw_fence->deadline.link,
				      &pos->deadline.link);
			return;
		}
	}

	list_add_tail(&hw_fence->deadline.link, &mgr->deadlines);
}

/**
 * xe_deadline_mgr_add_deadline() - Add deadline
 * @mgr: Deadline manager object
 * @fence: Fence with deadline (must be struct xe_hw_fence)
 * @deadline: Deadline for the fence
 *
 * Add a deadline for a fence. This may be called multiple times on a given
 * fence. It assumes upper layers only call this function multiple times if the
 * deadline is being reduced. If called after xe_deadline_mgr_remove_deadline,
 * this function is a NOP.
 */
void xe_deadline_mgr_add_deadline(struct xe_deadline_mgr *mgr,
				  struct dma_fence *fence,
				  ktime_t deadline)
{
	struct xe_hw_fence *hw_fence = to_xe_hw_fence(fence);

	if (mgr->state == XE_DEADLINE_MGR_STATE_UNSUPPORTED)
		return;

	guard(spinlock_irqsave)(&mgr->lock);

	if (hw_fence->deadline.time == XE_DEADLINE_DONE ||
	    deadline == XE_DEADLINE_DONE)
		return;

	xe_assert(gt_to_xe(mgr->q->gt),
		  hw_fence->deadline.time == XE_DEADLINE_NONE ||
		  deadline <= hw_fence->deadline.time);

	__xe_deadline_mgr_remove_deadline(mgr, hw_fence);
	__xe_deadline_mgr_add_deadline(mgr, hw_fence, deadline);
	__xe_deadline_mgr_update_deadline(mgr);
}

/**
 * xe_deadline_mgr_remove_deadline() - Remove deadline
 * @mgr: Deadline manager object
 * @fence: Fence with deadline (must be struct xe_hw_fence)
 *
 * Remove the deadline for a fence. This should be called exactly once after the
 * fence is signaled. After this function is called, future
 * xe_deadline_mgr_add_deadline calls are NOPs.
 */
void xe_deadline_mgr_remove_deadline(struct xe_deadline_mgr *mgr,
				     struct dma_fence *fence)
{
	if (mgr->state == XE_DEADLINE_MGR_STATE_UNSUPPORTED)
		return;

	guard(spinlock_irqsave)(&mgr->lock);

	xe_assert(gt_to_xe(mgr->q->gt), !dma_fence_is_container(fence));
	xe_assert(gt_to_xe(mgr->q->gt), dma_fence_is_signaled(fence));
	xe_assert(gt_to_xe(mgr->q->gt),
		  to_xe_hw_fence(fence)->deadline.time != XE_DEADLINE_DONE);

	__xe_deadline_mgr_remove_deadline(mgr, to_xe_hw_fence(fence));
}
