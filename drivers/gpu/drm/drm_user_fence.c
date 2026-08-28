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

	/* Release the extra reference stored for cancel() */
	if (ufence->fence)
		dma_fence_put(ufence->fence);

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

	queue_work(ufence->wq, &ufence->work);
	/*
	 * Put the transferred reference from add_callback. The stored
	 * reference in ufence->fence is released in drm_user_fence_destroy().
	 */
	dma_fence_put(fence);
}

/**
 * drm_user_fence_init - Initialize a user fence
 * @ufence: user fence to initialize
 * @wq: workqueue to run the worker on (must be ordered if sequencing matters)
 * @ops: driver operations
 *
 * Must be called from process context with a valid current->mm.
 * Grabs a reference to current->mm via mmgrab().
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
	ufence->fence = NULL;
	INIT_WORK(&ufence->work, drm_user_fence_work);
}
EXPORT_SYMBOL_GPL(drm_user_fence_init);

/**
 * drm_user_fence_add_callback - Attach a user fence to a dma-fence
 * @ufence: user fence
 * @fence: dma-fence to watch; ownership of this reference is transferred
 *         to the callback — caller must NOT put it afterward.
 *
 * When @fence signals, a work item is queued that calls ops->worker() with
 * the process MM active. If @fence has already signaled the work item is
 * queued immediately.
 *
 * An additional reference to @fence is stored internally in @ufence to
 * allow drm_user_fence_cancel() to be called safely without the caller
 * needing to hold a separate fence reference.
 *
 * On any return value the caller's fence reference is consumed.
 *
 * Return: 0 on success, negative errno on error.
 */
int drm_user_fence_add_callback(struct drm_user_fence *ufence,
				struct dma_fence *fence)
{
	int err;

	drm_user_fence_get(ufence);

	/* Extra ref stored for cancel() — lives until drm_user_fence_destroy() */
	ufence->fence = dma_fence_get(fence);

	err = dma_fence_add_callback(fence, &ufence->cb, drm_user_fence_cb);
	if (err == -ENOENT) {
		/* fence already signaled — queue work and release transferred ref */
		queue_work(ufence->wq, &ufence->work);
		dma_fence_put(fence);
		err = 0;
	} else if (err) {
		dma_fence_put(ufence->fence);
		ufence->fence = NULL;
		drm_user_fence_put(ufence);
		dma_fence_put(fence);
	}
	/* on success: transferred ref goes to drm_user_fence_cb */

	return err;
}
EXPORT_SYMBOL_GPL(drm_user_fence_add_callback);

/**
 * drm_user_fence_cancel - Cancel a pending user fence callback
 * @ufence: user fence
 *
 * Attempts to remove the pending callback before driver context teardown.
 * Must be called before the driver tears down its workqueue or ops.
 * The caller must hold a reference to @ufence across this call.
 *
 * If the callback has already fired this returns false and no additional
 * action is needed — the callback handles its own reference.
 *
 * If removal succeeds the callback reference is released internally.
 * The caller must still release its own separate reference via
 * drm_user_fence_put() when done with the object.
 *
 * This function is safe to call from atomic context as it only acquires
 * the dma-fence spinlock internally. If the caller also needs to wait
 * for the worker to finish, use drm_user_fence_cancel_sync() instead,
 * which may sleep.
 *
 * Return: true if callback was removed, false if it had already fired.
 */
bool drm_user_fence_cancel(struct drm_user_fence *ufence)
{
	struct dma_fence *fence = ufence->fence;

	if (!fence)
		return false;

	if (dma_fence_remove_callback(fence, &ufence->cb)) {
		/*
		 * Callback will not fire — release the transferred reference
		 * that would have been put by drm_user_fence_cb(). The stored
		 * reference in ufence->fence is released in destroy().
		 */
		dma_fence_put(fence);
		drm_user_fence_put(ufence);
		return true;
	}

	/* Callback already fired — it handled its own cleanup */
	return false;
}
EXPORT_SYMBOL_GPL(drm_user_fence_cancel);

/**
 * drm_user_fence_cancel_sync - Cancel callback and wait for worker to finish
 * @ufence: user fence
 *
 * Calls drm_user_fence_cancel() then cancel_work_sync() to guarantee
 * the worker has fully completed before returning.
 *
 * This function may sleep. Must not be called from atomic or interrupt
 * context. Use drm_user_fence_cancel() instead when sleeping is not
 * allowed.
 *
 * Drivers must call this during teardown before freeing any resources
 * accessed by ops->worker().
 */
void drm_user_fence_cancel_sync(struct drm_user_fence *ufence)
{
	drm_user_fence_cancel(ufence);
	if (cancel_work_sync(&ufence->work))
		drm_user_fence_put(ufence);
}
EXPORT_SYMBOL_GPL(drm_user_fence_cancel_sync);
