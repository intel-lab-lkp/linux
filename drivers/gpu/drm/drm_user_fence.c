// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 The Linux Foundation
 *
 * DRM user fence helper.
 *
 * Extends drm_work_fence with the ability to access userspace memory
 * from workqueue context by borrowing the process MM via kthread_use_mm().
 *
 * Drivers that need to write completion status to userspace (e.g., user
 * fences, signaling eventfds) embed drm_user_fence and implement
 * ops->worker() to do the actual write.
 *
 * Optionally, drivers may configure a per-signal compare via
 * drm_user_fence_set_compare(): work is skipped unless the value at a
 * userspace address matches the expected value at signal time.
 */

#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

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
	bool call_worker = true;

	if (mmget_not_zero(ufence->mm)) {
		mm = ufence->mm;
		kthread_use_mm(mm);
	}

	if (ufence->cmp_op != DRM_USER_FENCE_CMP_NONE &&
	    !(wfence->fence && wfence->fence->error)) {
		if (!mm) {
			call_worker = false;
		} else {
			__le64 raw;

			/*
			 * Use copy_from_user_nofault() to prevent a
			 * userfaultfd-registered page from blocking this
			 * workqueue thread indefinitely (DoS).
			 */
			if (copy_from_user_nofault(&raw, ufence->cmp_addr,
						   sizeof(raw))) {
				call_worker = false;
			} else {
				/* GPU writes LE; convert before comparing. */
				u64 cur_val = le64_to_cpu(raw);

				if (!drm_user_fence_cmp_match(cur_val,
							      ufence->cmp_value,
							      ufence->cmp_op))
					call_worker = false;
			}
		}
	}

	if (call_worker)
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
	ufence->mm        = current->mm;
	mmgrab(ufence->mm);
	ufence->ops       = ops;
	ufence->cmp_op    = DRM_USER_FENCE_CMP_NONE;
	ufence->cmp_addr  = NULL;
	ufence->cmp_value = 0;
}
EXPORT_SYMBOL_GPL(drm_user_fence_init);

/**
 * drm_user_fence_set_compare - Set per-signal compare filter
 * @ufence: user fence
 * @addr: 8-byte-aligned userspace address to read from at signal time
 * @value: expected value to compare against
 * @op: comparison operator; pass %DRM_USER_FENCE_CMP_NONE to disable
 *
 * When @op is not %DRM_USER_FENCE_CMP_NONE, the worker is only called
 * if the value at @addr matches @value according to @op. If the MM is
 * gone or the read fails, the worker is suppressed.
 *
 * Must only be called before drm_work_fence_add_callback().
 */
void drm_user_fence_set_compare(struct drm_user_fence *ufence,
				u64 __user *addr, u64 value,
				enum drm_user_fence_cmp op)
{
	/*
	 * get_user() of u64 is not atomic on 32-bit — caller should not
	 * reach here on non-64-bit kernels.
	 */
	if (WARN_ON_ONCE(!IS_ENABLED(CONFIG_64BIT)))
		return;

	if (op != DRM_USER_FENCE_CMP_NONE) {
		if (WARN_ON_ONCE(!addr ||
				 !IS_ALIGNED((unsigned long)addr, sizeof(u64))))
			return;
	}

	ufence->cmp_addr  = addr;
	ufence->cmp_value = value;
	ufence->cmp_op    = op;
}
EXPORT_SYMBOL_GPL(drm_user_fence_set_compare);
