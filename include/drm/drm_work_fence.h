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
	 * @writeback: Called from workqueue context when the dma-fence signals.
	 *
	 * Perform the deferred work here (copy_to_user, eventfd_signal, etc.).
	 * May sleep. Must not requeue the fence.
	 */
	void (*writeback)(struct drm_work_fence *wfence);

	/**
	 * @destroy: Called when the last reference is dropped.
	 * Free the containing structure here.
	 */
	void (*destroy)(struct drm_work_fence *wfence);
};

/**
 * struct drm_work_fence - DRM dma-fence-to-workqueue helper
 *
 * Embeddable base structure that queues a work item when a dma-fence signals.
 * Drivers embed this in their own structure and implement ops->writeback()
 * for the deferred work and ops->destroy() for cleanup.
 *
 * Call drm_work_fence_init() at creation and drm_work_fence_add_callback()
 * to arm on a dma-fence. Call drm_work_fence_cancel_sync() before teardown.
 */
struct drm_work_fence {
	/** @refcount: Reference count. */
	struct kref refcount;
	/** @wq: Workqueue on which to run the worker. */
	struct workqueue_struct *wq;
	/** @ops: Driver operations. */
	const struct drm_work_fence_ops *ops;
	/** @fence: The watched dma-fence; holds a single reference. */
	struct dma_fence *fence;
	/** @work: Work item queued when the fence signals. */
	struct work_struct work;
	/** @cb: Callback registered on the dma-fence. */
	struct dma_fence_cb cb;
};

void drm_work_fence_init(struct drm_work_fence *wfence,
			 struct workqueue_struct *wq,
			 const struct drm_work_fence_ops *ops);
void drm_work_fence_get(struct drm_work_fence *wfence);
void drm_work_fence_put(struct drm_work_fence *wfence);
int  drm_work_fence_add_callback(struct drm_work_fence *wfence,
				 struct dma_fence *fence);
bool drm_work_fence_cancel(struct drm_work_fence *wfence);
void drm_work_fence_cancel_sync(struct drm_work_fence *wfence);

#endif /* __DRM_WORK_FENCE_H__ */
