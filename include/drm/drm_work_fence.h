/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 The Linux Foundation
 */

#ifndef __DRM_WORK_FENCE_H__
#define __DRM_WORK_FENCE_H__

#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

struct drm_work_fence;

/**
 * struct drm_work_fence_ops - driver callbacks for a DRM work fence
 */
struct drm_work_fence_ops {
	/**
	 * @work: Called from workqueue context when the dma-fence signals.
	 * Perform any work that cannot run in IRQ context here.
	 */
	void (*work)(struct drm_work_fence *wfence);

	/**
	 * @destroy: Called when the last reference is dropped.
	 * Free the containing structure here.
	 */
	void (*destroy)(struct drm_work_fence *wfence);
};

/**
 * struct drm_work_fence - embeddable DRM work fence
 *
 * Provides a dma-fence callback that queues a work item when the fence
 * signals, allowing work that cannot run in IRQ context to be deferred
 * to a workqueue. Drivers embed this in their own structure.
 *
 * NOTE: This helper is a *consumer* of dma_fences only. It CANNOT be
 * used to implement dma_fence_ops. dma_fence callbacks are invoked
 * while holding the fence spinlock; work queued here may sleep
 * (copy_to_user, kthread_use_mm, eventfd_signal) and must not be
 * called under that spinlock.
 *
 * Call drm_work_fence_init() at creation and drm_work_fence_add_callback()
 * to arm. Call drm_work_fence_cancel_sync() before driver teardown.
 */
struct drm_work_fence {
	/** @refcount: Reference count. */
	struct kref refcount;
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
	const struct drm_work_fence_ops *ops;
};

void drm_work_fence_init(struct drm_work_fence *wfence,
			 struct workqueue_struct *wq,
			 const struct drm_work_fence_ops *ops);
void drm_work_fence_get(struct drm_work_fence *wfence);
void drm_work_fence_put(struct drm_work_fence *wfence);
int drm_work_fence_add_callback(struct drm_work_fence *wfence,
				struct dma_fence *fence);
bool drm_work_fence_cancel(struct drm_work_fence *wfence);
void drm_work_fence_cancel_sync(struct drm_work_fence *wfence);

#endif /* __DRM_WORK_FENCE_H__ */
