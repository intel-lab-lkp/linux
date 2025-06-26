// SPDX-License-Identifier: GPL-2.0 OR MIT
/**************************************************************************
 *
 * Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 *
 **************************************************************************/

#include "vmwgfx_drv.h"
#include <drm/drm_syncobj.h>

#define VMW_FENCE_WRAP (1 << 31)

struct vmw_fence_manager {
	struct vmw_private *dev_priv;
	spinlock_t lock;
	struct list_head fence_list;
	bool fifo_down;
	u64 ctx;
};

/**
 * struct vmw_event_fence_action - fence callback that delivers a DRM event.
 *
 * @base:  For use with dma_fence_add_callback(...)
 * @event: A pointer to the pending event.
 * @dev: Pointer to a struct drm_device so we can access the event stuff.
 * @tv_sec: If non-null, the variable pointed to will be assigned
 * current time tv_sec val when the fence signals.
 * @tv_usec: Must be set if @tv_sec is set, and the variable pointed to will
 * be assigned the current time tv_usec val when the fence signals.
 */
struct vmw_event_fence_action {
	struct dma_fence_cb base;

	struct drm_pending_event *event;
	struct drm_device *dev;

	uint32_t *tv_sec;
	uint32_t *tv_usec;
};

static struct vmw_fence_manager *
fman_from_fence(struct vmw_fence_obj *fence)
{
	return container_of(fence->base.lock, struct vmw_fence_manager, lock);
}

static void vmw_fence_obj_destroy(struct dma_fence *f)
{
	struct vmw_fence_obj *fence =
		container_of(f, struct vmw_fence_obj, base);
	struct vmw_fence_manager *fman = fman_from_fence(fence);

	if (!list_empty(&fence->head)) {
		/* The fence manager still has an implicit reference to this
		 * fence via the fence list if head is set. Because the lock is
		 * required to be held when the fence manager updates the fence
		 * list either the fence will have been removed after we get
		 * the lock below or we can safely remove it and the fence
		 * manager will never see it. This implies the fence is being
		 * deleted without being signaled which is dubious but valid
		 * if there are no callbacks. The dma_fence code that calls
		 * this hook will warn about deleted unsignaled with callbacks
		 * so no need to warn again here.
		 */
		spin_lock(&fman->lock);
		list_del_init(&fence->head);
		if (fence->waiter_added)
			vmw_seqno_waiter_remove(fman->dev_priv);
		spin_unlock(&fman->lock);
	}
	dma_fence_free(f);
}

static const char *vmw_fence_get_driver_name(struct dma_fence *f)
{
	return "vmwgfx";
}

static const char *vmw_fence_get_timeline_name(struct dma_fence *f)
{
	return "svga";
}

/* When we toggle signaling for the SVGA device there is a race period from
 * the time we first read the fence seqno to the time we enable interrupts.
 * If we miss the interrupt for a fence during this period its likely the driver
 * will stall. As a result we need to re-read the seqno after interrupts are
 * enabled. If interrupts were already enabled we just increment the number of
 * seqno waiters.
 */
static bool vmw_fence_enable_signaling(struct dma_fence *f)
{
	u32 seqno;
	struct vmw_fence_obj *fence =
		container_of(f, struct vmw_fence_obj, base);

	struct vmw_fence_manager *fman = fman_from_fence(fence);
	struct vmw_private *dev_priv = fman->dev_priv;
check_for_race:
	seqno = vmw_fence_read(dev_priv);
	if (seqno - fence->base.seqno < VMW_FENCE_WRAP) {
		if (fence->waiter_added) {
			vmw_seqno_waiter_remove(dev_priv);
			fence->waiter_added = false;
		}
		return false;
	} else if (!fence->waiter_added) {
		fence->waiter_added = true;
		if (vmw_seqno_waiter_add(dev_priv))
			goto check_for_race;
	}
	return true;
}

static u32 __vmw_fences_update(struct vmw_fence_manager *fman);

static const struct dma_fence_ops vmw_fence_ops = {
	.get_driver_name = vmw_fence_get_driver_name,
	.get_timeline_name = vmw_fence_get_timeline_name,
	.enable_signaling = vmw_fence_enable_signaling,
	.release = vmw_fence_obj_destroy,
};

struct vmw_fence_manager *vmw_fence_manager_init(struct vmw_private *dev_priv)
{
	struct vmw_fence_manager *fman = kzalloc(sizeof(*fman), GFP_KERNEL);

	if (unlikely(!fman))
		return NULL;

	fman->dev_priv = dev_priv;
	spin_lock_init(&fman->lock);
	INIT_LIST_HEAD(&fman->fence_list);
	fman->fifo_down = true;
	fman->ctx = dma_fence_context_alloc(1);

	return fman;
}

void vmw_fence_manager_takedown(struct vmw_fence_manager *fman)
{
	bool lists_empty;

	spin_lock(&fman->lock);
	lists_empty = list_empty(&fman->fence_list);
	spin_unlock(&fman->lock);

	BUG_ON(!lists_empty);
	kfree(fman);
}

static int vmw_fence_obj_init(struct vmw_fence_manager *fman,
			      struct vmw_fence_obj *fence, u32 seqno)
{
	int ret = 0;

	dma_fence_init(&fence->base, &vmw_fence_ops, &fman->lock,
		       fman->ctx, seqno);

	spin_lock(&fman->lock);
	if (unlikely(fman->fifo_down)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	/* This creates an implicit reference to the fence from the fence
	 * manager. It will be dropped when the fence is signaled which is
	 * expected to happen before deletion. The dtor has code to catch
	 * the rare deletion before signaling case.
	 */
	list_add_tail(&fence->head, &fman->fence_list);

out_unlock:
	spin_unlock(&fman->lock);
	return ret;

}

static u32 __vmw_fences_update(struct vmw_fence_manager *fman)
{
	struct vmw_fence_obj *fence, *next_fence;
	const bool cookie = dma_fence_begin_signalling();
	const u32 seqno = vmw_fence_read(fman->dev_priv);

	list_for_each_entry_safe(fence, next_fence, &fman->fence_list, head) {
		if (seqno - fence->base.seqno < VMW_FENCE_WRAP) {
			list_del_init(&fence->head);
			if (fence->waiter_added) {
				vmw_seqno_waiter_remove(fman->dev_priv);
				fence->waiter_added = false;
			}
			dma_fence_signal_locked(&fence->base);
		} else
			break;
	}
	dma_fence_end_signalling(cookie);
	atomic_set_release(&fman->dev_priv->last_read_seqno, seqno);
	return seqno;
}

u32 vmw_fences_update(struct vmw_fence_manager *fman)
{
	u32 seqno;
	spin_lock(&fman->lock);
	seqno = __vmw_fences_update(fman);
	spin_unlock(&fman->lock);
	return seqno;
}

bool vmw_fence_obj_signaled(struct vmw_fence_obj *fence)
{
	struct vmw_fence_manager *fman = fman_from_fence(fence);

	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->base.flags))
		return true;

	vmw_fences_update(fman);

	return dma_fence_is_signaled(&fence->base);
}

int vmw_fence_obj_wait(struct vmw_fence_obj *fence, bool lazy,
		       bool interruptible, unsigned long timeout)
{
	long ret = dma_fence_wait_timeout(&fence->base, interruptible, timeout);

	if (likely(ret > 0))
		return 0;
	else if (ret == 0)
		return -EBUSY;
	else
		return ret;
}

int vmw_fence_create(struct vmw_fence_manager *fman,
		     uint32_t seqno,
		     struct vmw_fence_obj **p_fence)
{
	struct vmw_fence_obj *fence;
	int ret;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (unlikely(!fence))
		return -ENOMEM;

	ret = vmw_fence_obj_init(fman, fence, seqno);
	if (unlikely(ret != 0))
		goto out_err_init;

	*p_fence = fence;
	return 0;

out_err_init:
	kfree(fence);
	return ret;
}

int vmw_user_fence_create(struct drm_file *file_priv,
			  struct vmw_fence_manager *fman,
			  uint32_t seqno,
			  struct vmw_fence_obj **p_fence,
			  uint32_t *p_handle)
{
	struct drm_syncobj *ufence;
	struct vmw_fence_obj *tmp;
	int ret;

	ret = vmw_fence_create(fman, seqno, &tmp);
	if (ret != 0)
		return ret;

	ret = drm_syncobj_create(&ufence, 0, &tmp->base);
	if (ret != 0) {
		vmw_fence_obj_unreference(&tmp);
		return ret;
	}

	ret = drm_syncobj_get_handle(file_priv, ufence, p_handle);
	drm_syncobj_put(ufence);
	if (ret != 0)
		vmw_fence_obj_unreference(&tmp);
	*p_fence = tmp;
	return ret;
}

/*
 * vmw_fence_fifo_down - signal all unsignaled fence objects.
 */

void vmw_fence_fifo_down(struct vmw_fence_manager *fman)
{
	int ret;

	/*
	 * The list may be altered while we traverse it, so always
	 * restart when we've released the fman->lock.
	 */

	spin_lock(&fman->lock);
	fman->fifo_down = true;
	while (!list_empty(&fman->fence_list)) {
		struct vmw_fence_obj *fence =
			list_entry(fman->fence_list.prev, struct vmw_fence_obj,
				   head);
		dma_fence_get(&fence->base);
		spin_unlock(&fman->lock);

		ret = vmw_fence_obj_wait(fence, false, false,
					 VMW_FENCE_WAIT_TIMEOUT);

		if (unlikely(ret != 0)) {
			list_del_init(&fence->head);
			dma_fence_signal(&fence->base);
		}

		BUG_ON(!list_empty(&fence->head));
		dma_fence_put(&fence->base);
		spin_lock(&fman->lock);
	}
	spin_unlock(&fman->lock);
}

void vmw_fence_fifo_up(struct vmw_fence_manager *fman)
{
	spin_lock(&fman->lock);
	fman->fifo_down = false;
	spin_unlock(&fman->lock);
}

int vmw_fence_obj_wait_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct drm_vmw_fence_wait_arg *arg =
	    (struct drm_vmw_fence_wait_arg *)data;
	unsigned long timeout;
	struct vmw_fence_obj *fence;
	int ret;
	uint64_t wait_timeout = ((uint64_t)arg->timeout_us * HZ);

	/*
	 * 64-bit division not present on 32-bit systems, so do an
	 * approximation. (Divide by 1000000).
	 */

	wait_timeout = (wait_timeout >> 20) + (wait_timeout >> 24) -
	  (wait_timeout >> 26);

	if (!arg->cookie_valid) {
		arg->cookie_valid = 1;
		arg->kernel_cookie = jiffies + wait_timeout;
	}

	ret = drm_syncobj_find_fence(file_priv, arg->handle, 0, 0,
				     (struct dma_fence **)&fence);
	if (ret != 0)
		return ret;

	timeout = jiffies;
	if (time_after_eq(timeout, (unsigned long)arg->kernel_cookie)) {
		ret = ((vmw_fence_obj_signaled(fence)) ?
		       0 : -EBUSY);
		goto out;
	}

	timeout = (unsigned long)arg->kernel_cookie - timeout;

	ret = vmw_fence_obj_wait(fence, arg->lazy, true, timeout);

out:
	vmw_fence_obj_unreference(&fence); // from find_fence

	/*
	 * Optionally unref the fence object.
	 */

	if (ret == 0 && (arg->wait_options & DRM_VMW_WAIT_OPTION_UNREF))
		ret = drm_syncobj_destroy(file_priv, arg->handle);
	return ret;
}

int vmw_fence_obj_signaled_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_vmw_fence_signaled_arg *arg =
		(struct drm_vmw_fence_signaled_arg *) data;
	struct vmw_fence_obj *fence;
	struct vmw_private *dev_priv = vmw_priv(dev);

	int ret = drm_syncobj_find_fence(file_priv, arg->handle, 0, 0,
					 (struct dma_fence **)&fence);
	if (ret != 0)
		return ret;


	arg->signaled = vmw_fence_obj_signaled(fence);

	arg->signaled_flags = arg->flags;
	arg->passed_seqno = atomic_read_acquire(&dev_priv->last_read_seqno);

	vmw_fence_obj_unreference(&fence); // from find_fence

	return 0;
}


int vmw_fence_obj_unref_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_vmw_fence_arg *arg =
		(struct drm_vmw_fence_arg *) data;

	return drm_syncobj_destroy(file_priv, arg->handle);
}

/**
 * vmw_event_fence_action_seq_passed
 *
 * @action: The struct vmw_fence_action embedded in a struct
 * vmw_event_fence_action.
 *
 * This function is called when the seqno of the fence where @action is
 * attached has passed. It queues the event on the submitter's event list.
 * This function is always called from atomic context.
 */
static void vmw_event_fence_action_seq_passed(struct dma_fence *f,
					      struct dma_fence_cb *cb)
{
	struct vmw_event_fence_action *eaction =
		container_of(cb, struct vmw_event_fence_action, base);
	struct drm_device *dev = eaction->dev;
	struct drm_pending_event *event = eaction->event;

	if (unlikely(event == NULL))
		return;

	spin_lock_irq(&dev->event_lock);

	if (likely(eaction->tv_sec != NULL)) {
		struct timespec64 ts;

		ktime_to_timespec64(f->timestamp);
		/* monotonic time, so no y2038 overflow */
		*eaction->tv_sec = ts.tv_sec;
		*eaction->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
	}

	drm_send_event_locked(dev, eaction->event);
	eaction->event = NULL;
	spin_unlock_irq(&dev->event_lock);
	dma_fence_put(f);
	kfree(eaction);
}

/**
 * vmw_event_fence_action_queue - Post an event for sending when a fence
 * object seqno has passed.
 *
 * @file_priv: The file connection on which the event should be posted.
 * @fence: The fence object on which to post the event.
 * @event: Event to be posted. This event should've been alloced
 * using k[mz]alloc, and should've been completely initialized.
 * @tv_sec: If non-null, the variable pointed to will be assigned
 * current time tv_sec val when the fence signals.
 * @tv_usec: Must be set if @tv_sec is set, and the variable pointed to will
 * be assigned the current time tv_usec val when the fence signals.
 * @interruptible: Interruptible waits if possible.
 *
 * As a side effect, the object pointed to by @event may have been
 * freed when this function returns. If this function returns with
 * an error code, the caller needs to free that object.
 */

int vmw_event_fence_action_queue(struct drm_file *file_priv,
				 struct vmw_fence_obj *fence,
				 struct drm_pending_event *event,
				 uint32_t *tv_sec,
				 uint32_t *tv_usec,
				 bool interruptible)
{
	struct vmw_event_fence_action *eaction;
	struct vmw_fence_manager *fman = fman_from_fence(fence);

	eaction = kzalloc(sizeof(*eaction), GFP_KERNEL);
	if (unlikely(!eaction))
		return -ENOMEM;

	eaction->event = event;
	eaction->dev = &fman->dev_priv->drm;
	eaction->tv_sec = tv_sec;
	eaction->tv_usec = tv_usec;

	vmw_fence_obj_reference(fence); // Dropped in CB
	if (dma_fence_add_callback(&fence->base, &eaction->base,
				   vmw_event_fence_action_seq_passed) < 0)
		vmw_event_fence_action_seq_passed(&fence->base, &eaction->base);
	return 0;
}

struct vmw_event_fence_pending {
	struct drm_pending_event base;
	struct drm_vmw_event_fence event;
};

static int vmw_event_fence_action_create(struct drm_file *file_priv,
				  struct vmw_fence_obj *fence,
				  uint32_t flags,
				  uint64_t user_data,
				  bool interruptible)
{
	struct vmw_event_fence_pending *event;
	struct vmw_fence_manager *fman = fman_from_fence(fence);
	struct drm_device *dev = &fman->dev_priv->drm;
	int ret;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (unlikely(!event)) {
		DRM_ERROR("Failed to allocate an event.\n");
		ret = -ENOMEM;
		goto out_no_space;
	}

	event->event.base.type = DRM_VMW_EVENT_FENCE_SIGNALED;
	event->event.base.length = sizeof(event->event);
	event->event.user_data = user_data;

	ret = drm_event_reserve_init(dev, file_priv, &event->base, &event->event.base);

	if (unlikely(ret != 0)) {
		DRM_ERROR("Failed to allocate event space for this file.\n");
		kfree(event);
		goto out_no_space;
	}

	if (flags & DRM_VMW_FE_FLAG_REQ_TIME)
		ret = vmw_event_fence_action_queue(file_priv, fence,
						   &event->base,
						   &event->event.tv_sec,
						   &event->event.tv_usec,
						   interruptible);
	else
		ret = vmw_event_fence_action_queue(file_priv, fence,
						   &event->base,
						   NULL,
						   NULL,
						   interruptible);
	if (ret != 0)
		goto out_no_queue;

	return 0;

out_no_queue:
	drm_event_cancel_free(dev, &event->base);
out_no_space:
	return ret;
}

int vmw_fence_event_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct vmw_private *dev_priv = vmw_priv(dev);
	struct drm_vmw_fence_event_arg *arg =
		(struct drm_vmw_fence_event_arg *) data;
	struct drm_syncobj  *ufence = NULL;
	struct vmw_fence_obj *fence = NULL;
	struct drm_vmw_fence_rep __user *user_fence_rep =
		(struct drm_vmw_fence_rep __user *)(unsigned long)arg->fence_rep;
	u32 handle = 0;
	int ret;

	/*
	 * Look up an existing fence object and if user-space
	 * wants a new reference add one.
	 */
	if (arg->handle) {
		ufence = drm_syncobj_find(file_priv, arg->handle);
		if (!ufence)
			return -ENOENT;

		fence = container_of(drm_syncobj_fence_get(ufence),
				     typeof(*fence), base);

		if (user_fence_rep != NULL) {
			ret = drm_syncobj_get_handle(file_priv, ufence, &handle);
			if (ret != 0) {
				DRM_ERROR("Failed to get a fence handle.\n");
				goto out_err;
			}
		}
	} else {
		ret = vmw_execbuf_fence_commands(file_priv, dev_priv, &fence,
						 (user_fence_rep) ?
						 &handle : NULL);
		if (unlikely(ret != 0)) {
			DRM_ERROR("Fence event failed to create fence.\n");
			return ret;
		}
	}

	BUG_ON(fence == NULL);

	ret = vmw_event_fence_action_create(file_priv, fence, arg->flags,
					    arg->user_data, true);
	if (ret != 0) {
		if (ret != -ERESTARTSYS)
			DRM_ERROR("Failed to attach event to fence.\n");
		if (handle)
			drm_syncobj_destroy(file_priv, handle);
		goto out_err;
	}

	ret = vmw_execbuf_copy_fence_user(dev_priv, file_priv, 0, user_fence_rep,
					  fence, handle, -1);
out_err:
	vmw_fence_obj_unreference(&fence);
	if (ufence)
		drm_syncobj_put(ufence);
	return ret;
}
