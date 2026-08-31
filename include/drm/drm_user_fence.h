/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 The Linux Foundation
 */

#ifndef __DRM_USER_FENCE_H__
#define __DRM_USER_FENCE_H__

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
	 * If @mm_ok is false, the process MM was already gone; the driver
	 * should log a warning and skip the userspace write.
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
};

void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops);

static inline void drm_user_fence_get(struct drm_user_fence *ufence)
{
	drm_work_fence_get(&ufence->base);
}

static inline void drm_user_fence_put(struct drm_user_fence *ufence)
{
	drm_work_fence_put(&ufence->base);
}

static inline int drm_user_fence_add_callback(struct drm_user_fence *ufence,
					      struct dma_fence *fence)
{
	return drm_work_fence_add_callback(&ufence->base, fence);
}

static inline bool drm_user_fence_cancel(struct drm_user_fence *ufence)
{
	return drm_work_fence_cancel(&ufence->base);
}

static inline void drm_user_fence_cancel_sync(struct drm_user_fence *ufence)
{
	drm_work_fence_cancel_sync(&ufence->base);
}

#endif /* __DRM_USER_FENCE_H__ */
