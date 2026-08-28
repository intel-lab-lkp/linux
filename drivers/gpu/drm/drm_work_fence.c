// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 The Linux Foundation
 *
 * Common DRM work fence helper.
 *
 * When a GPU dma-fence signals, drivers often need to perform work that
 * cannot run in IRQ context (e.g., memory allocation, copy_to_user,
 * eventfd_signal). This helper queues a work item when a dma-fence
 * signals, allowing that work to run safely in a workqueue context.
 *
 * NOTE: This helper consumes dma_fences but CANNOT implement
 * dma_fence_ops. Work items queued here may sleep; dma_fence_ops
 * callbacks are called under the fence spinlock and must not sleep.
 *
 * For work that additionally requires accessing userspace memory via
 * kthread_use_mm(), see drm_user_fence which builds on top of this.
 */

#include <linux/workqueue.h>

#include <drm/drm_work_fence.h>

static void drm_work_fence_destroy(struct kref *kref)
{
	struct drm_work_fence *wfence =
		container_of(kref, struct drm_work_fence, refcount);

	if (wfence->fence)
		dma_fence_put(wfence->fence);

	wfence->ops->destroy(wfence);
}

/**
 * drm_work_fence_get - Acquire a reference to a work fence
 * @wfence: work fence
 */
void drm_work_fence_get(struct drm_work_fence *wfence)
{
	kref_get(&wfence->refcount);
}
EXPORT_SYMBOL_GPL(drm_work_fence_get);

/**
 * drm_work_fence_put - Release a reference to a work fence
 * @wfence: work fence
 */
void drm_work_fence_put(struct drm_work_fence *wfence)
{
	kref_put(&wfence->refcount, drm_work_fence_destroy);
}
EXPORT_SYMBOL_GPL(drm_work_fence_put);

static void drm_work_fence_work(struct work_struct *w)
{
	struct drm_work_fence *wfence =
		container_of(w, struct drm_work_fence, work);

	wfence->ops->work(wfence);
	drm_work_fence_put(wfence);
}

static void drm_work_fence_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct drm_work_fence *wfence =
		container_of(cb, struct drm_work_fence, cb);

	queue_work(wfence->wq, &wfence->work);
	/*
	 * Put the transferred reference from add_callback. The stored
	 * reference in wfence->fence is released in drm_work_fence_destroy().
	 */
	dma_fence_put(fence);
}

/**
 * drm_work_fence_init - Initialize a work fence
 * @wfence: work fence to initialize
 * @wq: workqueue to run the worker on (must be ordered if sequencing matters)
 * @ops: driver operations
 */
void drm_work_fence_init(struct drm_work_fence *wfence,
			 struct workqueue_struct *wq,
			 const struct drm_work_fence_ops *ops)
{
	kref_init(&wfence->refcount);
	wfence->wq = wq;
	wfence->ops = ops;
	wfence->fence = NULL;
	INIT_WORK(&wfence->work, drm_work_fence_work);
}
EXPORT_SYMBOL_GPL(drm_work_fence_init);

/**
 * drm_work_fence_add_callback - Attach a work fence to a dma-fence
 * @wfence: work fence
 * @fence: dma-fence to watch; ownership of this reference is transferred
 *         to the callback — caller must NOT put it afterward.
 *
 * When @fence signals, a work item is queued that calls ops->work().
 * If @fence has already signaled, the work item is queued immediately.
 *
 * An additional reference to @fence is stored internally in @wfence to
 * allow drm_work_fence_cancel() to be called safely without the caller
 * needing to hold a separate fence reference.
 *
 * On any return value the caller's fence reference is consumed.
 *
 * Return: 0 on success, negative errno on error.
 */
int drm_work_fence_add_callback(struct drm_work_fence *wfence,
				struct dma_fence *fence)
{
	int err;

	drm_work_fence_get(wfence);
	wfence->fence = dma_fence_get(fence);

	err = dma_fence_add_callback(fence, &wfence->cb, drm_work_fence_cb);
	if (err == -ENOENT) {
		queue_work(wfence->wq, &wfence->work);
		dma_fence_put(fence);
		err = 0;
	} else if (err) {
		dma_fence_put(wfence->fence);
		wfence->fence = NULL;
		drm_work_fence_put(wfence);
		dma_fence_put(fence);
	}
	/* on success: transferred ref goes to drm_work_fence_cb */

	return err;
}
EXPORT_SYMBOL_GPL(drm_work_fence_add_callback);

/**
 * drm_work_fence_cancel - Cancel a pending work fence callback
 * @wfence: work fence
 *
 * Attempts to remove the pending callback before driver context teardown.
 * The caller must hold a reference to @wfence across this call.
 *
 * If the callback has already fired this returns false and all cleanup
 * has been handled internally.
 *
 * If removal succeeds the callback reference is released internally.
 * The caller must still release its own reference via drm_work_fence_put().
 *
 * This function is safe to call from atomic context as it only acquires
 * the dma-fence spinlock internally. If the caller also needs to wait
 * for the worker to finish, use drm_work_fence_cancel_sync() instead,
 * which may sleep.
 *
 * Return: true if callback was removed, false if it had already fired.
 */
bool drm_work_fence_cancel(struct drm_work_fence *wfence)
{
	struct dma_fence *fence = wfence->fence;

	if (!fence)
		return false;

	if (dma_fence_remove_callback(fence, &wfence->cb)) {
		dma_fence_put(fence);
		drm_work_fence_put(wfence);
		return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(drm_work_fence_cancel);

/**
 * drm_work_fence_cancel_sync - Cancel callback and wait for worker to finish
 * @wfence: work fence
 *
 * Calls drm_work_fence_cancel() then cancel_work_sync() to guarantee
 * the worker has fully completed before returning.
 *
 * This function may sleep. Must not be called from atomic or interrupt
 * context. Use drm_work_fence_cancel() instead when sleeping is not allowed.
 *
 * Drivers must call this during teardown before freeing any resources
 * accessed by ops->work().
 */
void drm_work_fence_cancel_sync(struct drm_work_fence *wfence)
{
	drm_work_fence_cancel(wfence);
	if (cancel_work_sync(&wfence->work))
		drm_work_fence_put(wfence);
}
EXPORT_SYMBOL_GPL(drm_work_fence_cancel_sync);
