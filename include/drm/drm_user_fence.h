/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 The Linux Foundation
 */

#ifndef __DRM_USER_FENCE_H__
#define __DRM_USER_FENCE_H__

#include <linux/dma-fence.h>

#include <drm/drm_work_fence.h>

struct drm_user_fence;

/**
 * struct drm_user_fence_ops - driver callbacks for a DRM user fence
 */
struct drm_user_fence_ops {
	/**
	 * @worker: Called from workqueue context with the process MM active.
	 *
	 * If @mm_ok is true, kthread_use_mm() is active and userspace memory
	 * (copy_to_user, etc.) may be accessed safely.
	 * If @mm_ok is false, the process MM was already gone; skip the
	 * userspace write.
	 *
	 * wake_up() or other post-signal housekeeping should also happen here.
	 */
	void (*worker)(struct drm_user_fence *ufence, bool mm_ok);

	/**
	 * @destroy: Called when the last reference is dropped.
	 * Free the containing structure here.
	 */
	void (*destroy)(struct drm_user_fence *ufence);
};

/**
 * enum drm_user_fence_cmp - compare operator for per-signal filtering
 */
enum drm_user_fence_cmp {
	DRM_USER_FENCE_CMP_NONE = 0,
	DRM_USER_FENCE_CMP_EQ,
	DRM_USER_FENCE_CMP_NEQ,
	DRM_USER_FENCE_CMP_GTE,
};

/**
 * struct drm_user_fence - DRM user fence with MM borrowing
 *
 * Extends drm_work_fence with kthread_use_mm() support for drivers
 * that need to access userspace memory when a GPU fence signals.
 * For work that does not need userspace memory access, use
 * drm_work_fence directly.
 *
 * Call drm_user_fence_init() at creation and drm_user_fence_add_callback()
 * to arm on a dma-fence. Call drm_user_fence_cancel_sync() before teardown.
 */
struct drm_user_fence {
	/** @base: Base work fence. Must be first. */
	struct drm_work_fence base;
	/** @mm: Process MM grabbed at init time. */
	struct mm_struct *mm;
	/** @ops: Driver operations. */
	const struct drm_user_fence_ops *ops;
	/** @cmp_addr: Userspace address to read for per-signal compare. */
	u64 __user *cmp_addr;
	/** @cmp_value: Expected value for per-signal compare. */
	u64 cmp_value;
	/** @cmp_op: Compare operator; DRM_USER_FENCE_CMP_NONE disables. */
	enum drm_user_fence_cmp cmp_op;
};

void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops);
void drm_user_fence_set_compare(struct drm_user_fence *ufence,
				u64 __user *addr, u64 value,
				enum drm_user_fence_cmp op);

/**
 * drm_user_fence_get - Acquire a reference to a user fence
 * @ufence: user fence
 */
static inline void drm_user_fence_get(struct drm_user_fence *ufence)
{
	drm_work_fence_get(&ufence->base);
}

/**
 * drm_user_fence_put - Release a reference to a user fence
 * @ufence: user fence
 */
static inline void drm_user_fence_put(struct drm_user_fence *ufence)
{
	drm_work_fence_put(&ufence->base);
}

/**
 * drm_user_fence_add_callback - Attach a user fence to a dma-fence
 * @ufence: user fence; one reference is consumed on any return value
 * @fence: dma-fence to watch; one reference is consumed on any return value
 *
 * When @fence signals, ops->worker() is called from workqueue context.
 * If @fence has already signaled, the worker is queued immediately.
 *
 * Return: 0 on success, negative errno on error.
 */
static inline int drm_user_fence_add_callback(struct drm_user_fence *ufence,
					      struct dma_fence *fence)
{
	return drm_work_fence_add_callback(&ufence->base, fence);
}

/**
 * drm_user_fence_cancel - Cancel a pending user fence callback
 * @ufence: user fence
 *
 * Return: true if callback was removed, false if it had already fired.
 */
static inline bool drm_user_fence_cancel(struct drm_user_fence *ufence)
{
	return drm_work_fence_cancel(&ufence->base);
}

/**
 * drm_user_fence_cancel_sync - Cancel callback and wait for worker to finish
 * @ufence: user fence
 *
 * Must be called during teardown before freeing any resources accessed
 * by ops->worker(). May sleep.
 */
static inline void drm_user_fence_cancel_sync(struct drm_user_fence *ufence)
{
	drm_work_fence_cancel_sync(&ufence->base);
}

/**
 * drm_user_fence_cmp_match - Test a value against the compare filter
 * @cur_val: value read from userspace (already converted from LE)
 * @cmp_value: expected value
 * @op: comparison operator
 *
 * Return: true if the comparison passes, false otherwise.
 */
static inline bool drm_user_fence_cmp_match(u64 cur_val, u64 cmp_value,
					    enum drm_user_fence_cmp op)
{
	switch (op) {
	case DRM_USER_FENCE_CMP_EQ:
		return cur_val == cmp_value;
	case DRM_USER_FENCE_CMP_NEQ:
		return cur_val != cmp_value;
	case DRM_USER_FENCE_CMP_GTE:
		return cur_val >= cmp_value;
	default:
		return false;
	}
}

#endif /* __DRM_USER_FENCE_H__ */
