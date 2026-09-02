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

#include <drm/drm_user_fence.h>

static void drm_user_fence_do_destroy(struct drm_work_fence *wfence)
{
	struct drm_user_fence *ufence =
		container_of(wfence, struct drm_user_fence, base);
	struct mm_struct *mm = ufence->mm;

	ufence->ops->destroy(ufence);
	mmdrop(mm);
}

static void drm_user_fence_do_work(struct drm_work_fence *wfence)
{
	struct drm_user_fence *ufence =
		container_of(wfence, struct drm_user_fence, base);
	struct mm_struct *mm = NULL;

	if (mmget_not_zero(ufence->mm)) {
		mm = ufence->mm;
		kthread_use_mm(mm);
	}

	ufence->ops->worker(ufence, !!mm);

	if (mm) {
		kthread_unuse_mm(mm);
		mmput_async(mm);
	}
}

static const struct drm_work_fence_ops drm_user_fence_wfence_ops = {
	.writeback = drm_user_fence_do_work,
	.destroy   = drm_user_fence_do_destroy,
};

/**
 * drm_user_fence_init - Initialize a user fence
 * @ufence: user fence to initialize
 * @wq: workqueue on which to run the worker
 * @ops: driver operations
 *
 * Must be called from process context with a valid current->mm.
 * Grabs a reference to current->mm via mmgrab().
 */
void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops)
{
	drm_work_fence_init(&ufence->base, wq, &drm_user_fence_wfence_ops);
	ufence->mm = current->mm;
	mmgrab(ufence->mm);
	ufence->ops = ops;
}
EXPORT_SYMBOL_GPL(drm_user_fence_init);
