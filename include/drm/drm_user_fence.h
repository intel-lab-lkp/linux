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
	 * may be accessed safely. If @mm_ok is false, the process MM was
	 * already gone; skip the userspace write.
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
 * struct drm_user_fence - DRM user fence with MM borrowing
 *
 * Extends drm_work_fence with kthread_use_mm() support for drivers
 * that need to access userspace memory when a GPU fence signals.
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
};

void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops);

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
 * @ufence: user fence; caller retains their reference and must release
 *          it via drm_user_fence_put() when no longer needed
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
 * Must be called during teardown before freeing resources. May sleep.
 */
static inline void drm_user_fence_cancel_sync(struct drm_user_fence *ufence)
{
	drm_work_fence_cancel_sync(&ufence->base);
}

#endif /* __DRM_USER_FENCE_H__ */
