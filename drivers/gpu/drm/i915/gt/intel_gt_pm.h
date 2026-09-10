/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_GT_PM_H
#define INTEL_GT_PM_H

#include <linux/types.h>

#include "intel_gt_types.h"
#include "intel_wakeref.h"

static inline bool intel_gt_pm_is_awake(const struct intel_gt *gt)
{
	return intel_wakeref_is_active(&gt->wakeref);
}

static inline void intel_gt_pm_get_untracked(struct intel_gt *gt)
{
	intel_wakeref_get(&gt->wakeref);
}

static inline intel_wakeref_t intel_gt_pm_get(struct intel_gt *gt)
{
	intel_gt_pm_get_untracked(gt);
	return intel_wakeref_track(&gt->wakeref);
}

static inline void __intel_gt_pm_get(struct intel_gt *gt)
{
	__intel_wakeref_get(&gt->wakeref);
}

static inline intel_wakeref_t intel_gt_pm_get_if_awake(struct intel_gt *gt)
{
	if (!intel_wakeref_get_if_active(&gt->wakeref))
		return NULL;

	return intel_wakeref_track(&gt->wakeref);
}

static inline void intel_gt_pm_might_get(struct intel_gt *gt)
{
	intel_wakeref_might_get(&gt->wakeref);
}

static inline void intel_gt_pm_put_untracked(struct intel_gt *gt)
{
	intel_wakeref_put(&gt->wakeref);
}

static inline void intel_gt_pm_put(struct intel_gt *gt, intel_wakeref_t handle)
{
	intel_wakeref_untrack(&gt->wakeref, handle);
	intel_gt_pm_put_untracked(gt);
}

static inline void intel_gt_pm_put_async_untracked(struct intel_gt *gt)
{
	intel_wakeref_put_async(&gt->wakeref);
}

static inline void intel_gt_pm_might_put(struct intel_gt *gt)
{
	intel_wakeref_might_put(&gt->wakeref);
}

static inline void intel_gt_pm_put_async(struct intel_gt *gt, intel_wakeref_t handle)
{
	intel_wakeref_untrack(&gt->wakeref, handle);
	intel_gt_pm_put_async_untracked(gt);
}

/**
 * intel_gt_pm_put_delay - release the GT wakeref, deferred by a timer
 *
 * @gt: pointer to the gt
 * @handle: the handle returned by the matching intel_gt_pm_get*()
 * @delay: delay, in jiffies, before the release is processed
 *
 * As intel_gt_pm_put_async(), except that dropping the last reference arms a
 * timer instead of queueing the release immediately.
 *
 * intel_gt_pm_put_async() ends up in mod_delayed_work() with a zero delay,
 * which queues the work straight away and so wakes a workqueue worker through
 * try_to_wake_up(). That deadlocks if the caller already holds a runqueue
 * lock, since try_to_wake_up() then tries to take it again. Callers which can
 * run from inside the scheduler must use this variant with a non-zero @delay,
 * which takes the add_timer_on() path and never wakes a task.
 */
static inline void intel_gt_pm_put_delay(struct intel_gt *gt, intel_wakeref_t handle,
					 unsigned long delay)
{
	intel_wakeref_untrack(&gt->wakeref, handle);
	intel_wakeref_put_delay(&gt->wakeref, delay);
}

#define with_intel_gt_pm(gt, wf) \
	for ((wf) = intel_gt_pm_get(gt); (wf); intel_gt_pm_put((gt), (wf)), (wf) = NULL)

/**
 * with_intel_gt_pm_if_awake - if GT is PM awake, get a reference to prevent
 *	it to sleep, run some code and then asynchrously put the reference
 *	away.
 *
 * @gt: pointer to the gt
 * @wf: pointer to a temporary wakeref.
 */
#define with_intel_gt_pm_if_awake(gt, wf) \
	for ((wf) = intel_gt_pm_get_if_awake(gt); (wf); intel_gt_pm_put_async((gt), (wf)), (wf) = NULL)

static inline int intel_gt_pm_wait_for_idle(struct intel_gt *gt)
{
	return intel_wakeref_wait_for_idle(&gt->wakeref);
}

void intel_gt_pm_init_early(struct intel_gt *gt);
void intel_gt_pm_init(struct intel_gt *gt);
void intel_gt_pm_fini(struct intel_gt *gt);

void intel_gt_suspend_prepare(struct intel_gt *gt);
void intel_gt_suspend_late(struct intel_gt *gt);
int intel_gt_resume(struct intel_gt *gt);
void intel_gt_resume_early(struct intel_gt *gt);

void intel_gt_runtime_suspend(struct intel_gt *gt);
int intel_gt_runtime_resume(struct intel_gt *gt);

ktime_t intel_gt_get_awake_time(const struct intel_gt *gt);

#define INTEL_WAKEREF_MOCK_GT ERR_PTR(-ENODEV)

static inline bool is_mock_gt(const struct intel_gt *gt)
{
	BUILD_BUG_ON(INTEL_WAKEREF_DEF == INTEL_WAKEREF_MOCK_GT);

	return I915_SELFTEST_ONLY(gt->awake == INTEL_WAKEREF_MOCK_GT);
}

#endif /* INTEL_GT_PM_H */
