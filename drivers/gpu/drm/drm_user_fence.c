// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 The Linux Foundation
 *
 * Common DRM user fence helper.
 *
 * When a GPU dma-fence signals, drivers often need to write a value to a
 * userspace VA or notify userspace via an eventfd. Both operations require
 * a valid process MM, which is not available in IRQ context.
 *
 * This helper queues a work item on fence signal. The work item borrows the
 * process MM via kthread_use_mm() and calls ops->worker(), which the driver
 * implements to perform the actual userspace access.
 */

#include <linux/kthread.h>
#include <linux/sched/mm.h>
#include <linux/workqueue.h>

#include <drm/drm_user_fence.h>

static void drm_user_fence_destroy(struct kref *kref)
{
	struct drm_user_fence *ufence =
		container_of(kref, struct drm_user_fence, refcount);

	mmdrop(ufence->mm);
	ufence->ops->destroy(ufence);
}

/**
 * drm_user_fence_get - Acquire a reference to a user fence
 * @ufence: user fence
 */
void drm_user_fence_get(struct drm_user_fence *ufence)
{
	kref_get(&ufence->refcount);
}
EXPORT_SYMBOL_GPL(drm_user_fence_get);

/**
 * drm_user_fence_put - Release a reference to a user fence
 * @ufence: user fence
 */
void drm_user_fence_put(struct drm_user_fence *ufence)
{
	kref_put(&ufence->refcount, drm_user_fence_destroy);
}
EXPORT_SYMBOL_GPL(drm_user_fence_put);

static void drm_user_fence_work(struct work_struct *w)
{
	struct drm_user_fence *ufence =
		container_of(w, struct drm_user_fence, work);
	bool mm_ok = false;

	if (mmget_not_zero(ufence->mm)) {
		kthread_use_mm(ufence->mm);
		mm_ok = true;
	}

	ufence->ops->worker(ufence, mm_ok);

	if (mm_ok) {
		kthread_unuse_mm(ufence->mm);
		mmput(ufence->mm);
	}

	drm_user_fence_put(ufence);
}

static void drm_user_fence_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct drm_user_fence *ufence =
		container_of(cb, struct drm_user_fence, cb);

	INIT_WORK(&ufence->work, drm_user_fence_work);
	queue_work(ufence->wq, &ufence->work);
}

/**
 * drm_user_fence_init - Initialize a user fence
 * @ufence: user fence to initialize
 * @wq: workqueue to run the worker on (must be ordered if sequencing matters)
 * @ops: driver operations
 *
 * Must be called from process context. Grabs a reference to current->mm.
 */
void drm_user_fence_init(struct drm_user_fence *ufence,
			 struct workqueue_struct *wq,
			 const struct drm_user_fence_ops *ops)
{
	kref_init(&ufence->refcount);
	ufence->mm = current->mm;
	mmgrab(ufence->mm);
	ufence->wq = wq;
	ufence->ops = ops;
}
EXPORT_SYMBOL_GPL(drm_user_fence_init);

/**
 * drm_user_fence_add_callback - Attach a user fence to a dma-fence
 * @ufence: user fence
 * @fence: dma-fence to watch; caller retains ownership of this reference
 *
 * When @fence signals, a work item is queued that calls ops->worker() with
 * the process MM active. If @fence has already signaled the work item is
 * queued immediately.
 *
 * Return: 0 on success, negative errno on error.
 */
int drm_user_fence_add_callback(struct drm_user_fence *ufence,
				struct dma_fence *fence)
{
	int err;

	drm_user_fence_get(ufence);
	err = dma_fence_add_callback(fence, &ufence->cb, drm_user_fence_cb);
	if (err == -ENOENT) {
		/* fence already signaled — queue work immediately */
		INIT_WORK(&ufence->work, drm_user_fence_work);
		queue_work(ufence->wq, &ufence->work);
		err = 0;
	} else if (err) {
		drm_user_fence_put(ufence);
	}

	return err;
}
EXPORT_SYMBOL_GPL(drm_user_fence_add_callback);
