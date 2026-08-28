/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 The Linux Foundation
 */

#ifndef __DRM_USER_FENCE_H__
#define __DRM_USER_FENCE_H__

#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

struct drm_user_fence;

/**
 * struct drm_user_fence_ops - driver callbacks for a DRM user fence
 */
struct drm_user_fence_ops {
	/**
	 * @worker: Called from workqueue context.
	 *
	 * If @mm_ok is true, kthread_use_mm() is active and userspace memory
	 * (copy_to_user, eventfd_signal, etc.) may be accessed safely.
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
 * struct drm_user_fence - embeddable DRM user fence
 *
 * Drivers embed this in their own structure and implement
 * &drm_user_fence_ops. Call drm_user_fence_init() at creation and
 * drm_user_fence_add_callback() to arm on a dma-fence.
 * Call drm_user_fence_cancel_sync() before driver teardown.
 */
struct drm_user_fence {
	/** @refcount: Reference count. */
	struct kref refcount;
	/** @mm: Process MM grabbed at init time. */
	struct mm_struct *mm;
	/** @work: Work item queued when the dma-fence signals. */
	struct work_struct work;
	/** @cb: dma-fence callback. */
	struct dma_fence_cb cb;
	/**
	 * @fence: Extra reference held for safe cancel(). Set during
	 * add_callback, released in destroy().
	 */
	struct dma_fence *fence;
	/** @wq: Workqueue to run @work on. */
	struct workqueue_struct *wq;
	/** @ops: Driver operations. */
	const struct drm_user_fence_ops *ops;
};

void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops);
void drm_user_fence_get(struct drm_user_fence *ufence);
void drm_user_fence_put(struct drm_user_fence *ufence);
int drm_user_fence_add_callback(struct drm_user_fence *ufence,
				struct dma_fence *fence);
bool drm_user_fence_cancel(struct drm_user_fence *ufence);
void drm_user_fence_cancel_sync(struct drm_user_fence *ufence);

