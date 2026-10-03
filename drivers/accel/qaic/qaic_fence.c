// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/sprintf.h>

#include "qaic.h"

static const char *qaic_fence_get_driver_name(struct dma_fence *f)
{
	return QAIC_NAME;
}

static const char *qaic_fence_get_timeline_name(struct dma_fence *f)
{
	struct qaic_fence *qfence = container_of(f, struct qaic_fence, base);

	if (!qfence->timeline_name[0])
		scnprintf(qfence->timeline_name, ARRAY_SIZE(qfence->timeline_name), "D%02u",
			 qfence->dbc_id);
	return qfence->timeline_name;
}

static const struct dma_fence_ops qaic_fence_ops = {
	.get_driver_name = qaic_fence_get_driver_name,
	.get_timeline_name = qaic_fence_get_timeline_name,
};

static void qaic_fence_sync_cb_func(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct qaic_bo *bo = container_of(cb, struct qaic_bo, cb);
	struct qaic_device *qdev;

	if (f->error)
		return;

	qdev = bo->dbc->qdev;
	dma_sync_sgtable_for_cpu(&qdev->pdev->dev, bo->sgt, bo->dir);
	bo->perf_stats.req_processed_ts = ktime_to_ns(f->timestamp);
}

int qaic_fence_wait(struct dma_fence *fence, signed long timeout)
{
	int ret;

	timeout = dma_fence_wait_timeout(fence, true, timeout);
	if (!timeout) { /* Wait timed out */
		ret = -ETIMEDOUT;
	} else if (timeout < 0) { /* Wait interrupted */
		ret = -EINTR;
	} else { /* Wait completed */
		ret = dma_fence_get_status(fence);
		WARN_ON_ONCE(ret == 0);
		if (!ret) /* Fence not signaled yet */
			ret = -EBUSY;
		else if (ret > 0)  /* Fence is signaled successfully */
			ret = 0;
		/* otherwise ret already contains the error code of the fence */
	}
	return ret;
}

static struct qaic_fence *qaic_create_fence(struct qaic_bo *bo)
{
	struct qaic_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&fence->lock);
	fence->dbc_id = bo->dbc->id;
	dma_fence_init(&fence->base, &qaic_fence_ops, &fence->lock, bo->fence_context, 1);

	return fence;
}

/* Caller should be holding bo->lock */
static int qaic_set_bo_resv_fence(struct qaic_bo *bo, struct qaic_fence *fence)
{
	int ret;

	ret = dma_resv_reserve_fences(bo->base.resv, 1);
	if (ret)
		goto err_out_fence;

	/*
	 * Add a fence in dma-buf reservation object for this BO,
	 * enables buffer sharing across devices.
	 */
	dma_resv_add_fence(bo->base.resv, &fence->base,
			   dma_resv_usage_rw(bo->dir == DMA_TO_DEVICE));

	ret = dma_fence_add_callback(&fence->base, &bo->cb, qaic_fence_sync_cb_func);
	if (ret)
		goto err_out_fence;

	return ret;

err_out_fence:
	dma_fence_put(&fence->base);
	bo->fence = NULL;
	return ret;
}

/* Caller should be holding bo->lock */
void qaic_remove_bo_fence(struct qaic_bo *bo)
{
	if (bo->fence) {
		if (!dma_fence_is_signaled(&bo->fence->base)) {
			dma_fence_set_error(&bo->fence->base, -ECANCELED);
			dma_fence_signal(&bo->fence->base);
		}
		dma_fence_put(&bo->fence->base);
	}
	bo->fence = NULL;
}

/* Caller should be holding bo->lock */
static void qaic_replace_bo_fence(struct qaic_bo *bo, struct qaic_fence *fence)
{
	qaic_remove_bo_fence(bo);
	bo->fence = fence;
}

/* Caller should be holding bo->lock */
int qaic_acquire_bo_fence(struct qaic_bo *bo)
{
	struct qaic_fence *fence;
	int ret = 0;

	fence = qaic_create_fence(bo);
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	qaic_replace_bo_fence(bo, fence);
	ret = qaic_set_bo_resv_fence(bo, fence);

	return ret;
}
