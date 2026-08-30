// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 The Linux Foundation
 *
 * DRM user fence — extends drm_work_fence with kthread_use_mm() support.
 *
 * Use this when a GPU fence signals and work needs to access userspace
 * memory (copy_to_user, fault-able operations) from a kthread context.
 * For work that does not require userspace memory access, use
 * drm_work_fence directly.
 */

#include <linux/kthread.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>

#include <drm/drm_user_fence.h>

static bool drm_user_fence_cmp_match(u64 cur_val, u64 expected,
				     enum drm_user_fence_cmp op)
{
	switch (op) {
	case DRM_USER_FENCE_CMP_EQ:
		return cur_val == expected;
	case DRM_USER_FENCE_CMP_NE:
		return cur_val != expected;
	case DRM_USER_FENCE_CMP_GT:
		return cur_val > expected;
	case DRM_USER_FENCE_CMP_GE:
		return cur_val >= expected;
	case DRM_USER_FENCE_CMP_LT:
		return cur_val < expected;
	case DRM_USER_FENCE_CMP_LE:
		return cur_val <= expected;
	default:
		return true;
	}
}

static void drm_user_fence_do_work(struct drm_work_fence *wfence)
{
	struct drm_user_fence *ufence =
		container_of(wfence, struct drm_user_fence, base);
	bool mm_ok = false;
	bool call_worker = true;

	if (mmget_not_zero(ufence->mm)) {
		kthread_use_mm(ufence->mm);
		mm_ok = true;
	}

	/*
	 * Per-signal comparison: read a value from userspace and compare
	 * with the expected value. Skip ops->worker if the condition is
	 * not met. Drivers that do not need filtering leave cmp_addr NULL.
	 *
	 * If the MM is gone and cmp_addr is set we cannot perform the
	 * comparison, so skip the worker rather than calling it without
	 * having verified the condition.
	 */
	if (ufence->cmp_op != DRM_USER_FENCE_CMP_NONE) {
		if (!mm_ok) {
			call_worker = false;
		} else {
			u64 cur_val;

			if (get_user(cur_val, ufence->cmp_addr) ||
			    !drm_user_fence_cmp_match(cur_val,
						      ufence->cmp_value,
						      ufence->cmp_op))
				call_worker = false;
		}
	}

	if (call_worker)
		ufence->ops->worker(ufence, mm_ok);

	if (mm_ok) {
		kthread_unuse_mm(ufence->mm);
		mmput_async(ufence->mm);
	}
}

static void drm_user_fence_do_destroy(struct drm_work_fence *wfence)
{
	struct drm_user_fence *ufence =
		container_of(wfence, struct drm_user_fence, base);

	mmdrop(ufence->mm);
	ufence->ops->destroy(ufence);
}

static const struct drm_work_fence_ops drm_user_fence_wf_ops = {
	.work    = drm_user_fence_do_work,
	.destroy = drm_user_fence_do_destroy,
};

/**
 * drm_user_fence_init - Initialize a user fence
 * @ufence: user fence to initialize
 * @wq: workqueue to run the worker on (must be ordered if sequencing matters)
 * @ops: driver operations
 *
 * Must be called from process context with a valid current->mm.
 * Grabs a reference to current->mm via mmgrab().
 */
void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops)
{
	drm_work_fence_init(&ufence->base, wq, &drm_user_fence_wf_ops);
	ufence->mm = current->mm;
	mmgrab(ufence->mm);
	ufence->ops = ops;
	ufence->cmp_addr = NULL;
	ufence->cmp_value = 0;
	ufence->cmp_op = DRM_USER_FENCE_CMP_NONE;
}
EXPORT_SYMBOL_GPL(drm_user_fence_init);

/**
 * drm_user_fence_set_compare - Configure per-signal value comparison
 * @ufence: user fence
 * @addr: userspace VA to read when the fence signals
 * @value: expected value to compare against
 * @op: comparison operator (see &enum drm_user_fence_cmp)
 *
 * When set, drm_user_fence reads @addr via get_user() each time the
 * fence signals and calls ops->worker() only if the comparison passes.
 * This enables per-signal filtering without open-coding the read+compare
 * pattern in each driver.
 *
 * Must be called after drm_user_fence_init() and before
 * drm_user_fence_add_callback().
 */
void drm_user_fence_set_compare(struct drm_user_fence *ufence,
				u64 __user *addr, u64 value,
				enum drm_user_fence_cmp op)
{
	if (WARN_ON(op != DRM_USER_FENCE_CMP_NONE && !addr))
		return;

	ufence->cmp_addr = addr;
	ufence->cmp_value = value;
	ufence->cmp_op = op;
}
EXPORT_SYMBOL_GPL(drm_user_fence_set_compare);
