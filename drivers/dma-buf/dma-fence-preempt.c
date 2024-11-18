// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/dma-fence-preempt.h>
#include <linux/dma-resv.h>

static void dma_fence_preempt_work_func(struct work_struct *w)
{
	bool cookie = dma_fence_begin_signalling();
	struct dma_fence_preempt *pfence =
		container_of(w, typeof(*pfence), work);
	const struct dma_fence_preempt_ops *ops = pfence->ops;
	int err = pfence->base.error;

	if (!err) {
		err = ops->preempt_wait(pfence);
		if (err)
			dma_fence_set_error(&pfence->base, err);
	}

	dma_fence_signal(&pfence->base);
	ops->preempt_finished(pfence);

	dma_fence_end_signalling(cookie);
}

static const char *
dma_fence_preempt_get_driver_name(struct dma_fence *fence)
{
	return "dma_fence_preempt";
}

static const char *
dma_fence_preempt_get_timeline_name(struct dma_fence *fence)
{
	return "ordered";
}

static void dma_fence_preempt_issue(struct dma_fence_preempt *pfence)
{
	int err;

	err = pfence->ops->preempt(pfence);
	if (err)
		dma_fence_set_error(&pfence->base, err);

	queue_work(pfence->wq, &pfence->work);
}

static void dma_fence_preempt_cb(struct dma_fence *fence,
				 struct dma_fence_cb *cb)
{
	struct dma_fence_preempt *pfence =
		container_of(cb, typeof(*pfence), cb);

	dma_fence_preempt_issue(pfence);
}

static void dma_fence_preempt_delay(struct dma_fence_preempt *pfence)
{
	struct dma_fence *fence;
	int err;

	fence = pfence->ops->preempt_delay(pfence);
	if (WARN_ON_ONCE(!fence || IS_ERR(fence)))
		return;

	err = dma_fence_add_callback(fence, &pfence->cb, dma_fence_preempt_cb);
	if (err == -ENOENT)
		dma_fence_preempt_issue(pfence);
}

static bool dma_fence_preempt_enable_signaling(struct dma_fence *fence)
{
	struct dma_fence_preempt *pfence =
		container_of(fence, typeof(*pfence), base);

	if (pfence->ops->preempt_delay)
		dma_fence_preempt_delay(pfence);
	else
		dma_fence_preempt_issue(pfence);

	return true;
}

static const struct dma_fence_ops preempt_fence_ops = {
	.get_driver_name = dma_fence_preempt_get_driver_name,
	.get_timeline_name = dma_fence_preempt_get_timeline_name,
	.enable_signaling = dma_fence_preempt_enable_signaling,
};

/**
 * dma_fence_is_preempt() - Is preempt fence
 *
 * @fence: Preempt fence
 *
 * Return: True if preempt fence, False otherwise
 */
bool dma_fence_is_preempt(const struct dma_fence *fence)
{
	return fence->ops == &preempt_fence_ops;
}
EXPORT_SYMBOL(dma_fence_is_preempt);

/**
 * dma_fence_preempt_init() - Initial preempt fence
 *
 * @fence: Preempt fence
 * @ops: Preempt fence operations
 * @wq: Work queue for preempt wait, should have WQ_MEM_RECLAIM set
 * @context: Fence context
 * @seqno: Fence seqence number
 */
void dma_fence_preempt_init(struct dma_fence_preempt *fence,
			    const struct dma_fence_preempt_ops *ops,
			    struct workqueue_struct *wq,
			    u64 context, u64 seqno)
{
	/*
	 * XXX: We really want to check wq for WQ_MEM_RECLAIM here but
	 * workqueue_struct is private.
	 */

	fence->ops = ops;
	fence->wq = wq;
	INIT_WORK(&fence->work, dma_fence_preempt_work_func);
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &preempt_fence_ops,
		       &fence->lock, context, seqno);
}
EXPORT_SYMBOL(dma_fence_preempt_init);
