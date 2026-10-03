// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/dma-fence-array.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/slab.h>
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
		scnprintf(qfence->timeline_name, ARRAY_SIZE(qfence->timeline_name),
			  "D%02u-JOB%05x", qfence->dbc_id, qfence->job_id);
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

struct qaic_fence *qaic_create_irq_fence(struct qaic_bo *bo, u64 seqno, u16 job_id)
{
	struct qaic_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&fence->lock);
	fence->dbc_id = bo->dbc->id;
	fence->job_id = job_id;
	dma_fence_init(&fence->base, &qaic_fence_ops, &fence->lock, bo->fence_context, seqno);

	return fence;
}

static int qaic_create_bo_fence(struct qaic_bo *bo, struct dma_fence **output_fence,
				int job_count, struct list_head *job_list)
{
	struct dma_fence_array *fence_arr;
	struct dma_fence **job_fences;
	struct qaic_job *job;
	int ret, i = 0;

	job_fences = kcalloc(job_count, sizeof(*job_fences), GFP_KERNEL);
	if (!job_fences)
		return -ENOMEM;

	ret = dma_resv_reserve_fences(bo->base.resv, job_count);
	if (ret)
		goto job_err;

	/* Add all the fences that we need to wait on to resv */
	list_for_each_entry_reverse(job, job_list, queue) {
		if (i < job_count) {
			job_fences[i] = dma_fence_get(&job->irq_fence->base);
			dma_resv_add_fence(bo->base.resv, job_fences[i],
					   dma_resv_usage_rw(bo->dir == DMA_TO_DEVICE));
			i++;
		} else {
			break;
		}
	}

	/* When successful, job_fences and the fences it contains are now owned by fence_arr */
	fence_arr = dma_fence_array_create(job_count, job_fences, bo->fence_context, 1);
	if (!fence_arr) {
		ret = -ENOMEM;
		goto fence_err;
	}

	/* Output fence array so bo will only have one fence to wait on */
	*output_fence = &fence_arr->base;
	return 0;
fence_err:
	for (i = 0; i < job_count; i++)
		dma_fence_put(job_fences[i]);
job_err:
	kfree(job_fences);
	return ret;

}

/* Caller should be holding bo->lock */
void qaic_remove_bo_fence(struct qaic_bo *bo)
{
	if (bo->fence) {
		if (!dma_fence_is_signaled(bo->fence)) {
			dma_fence_set_error(bo->fence, -ECANCELED);
			dma_fence_signal(bo->fence);
		}
		dma_fence_put(bo->fence);
	}
	bo->fence = NULL;
}

/* Caller should be holding bo->lock */
static void qaic_replace_bo_fence(struct qaic_bo *bo, struct dma_fence *fence)
{
	qaic_remove_bo_fence(bo);
	bo->fence = fence;
}

/* Caller should be holding bo->lock */
int qaic_acquire_bo_fence(struct qaic_bo *bo, int job_count, struct list_head *job_list)
{
	struct dma_fence *fence;
	int ret;

	ret = qaic_create_bo_fence(bo, &fence, job_count, job_list);
	if (ret)
		return ret;

	ret = dma_fence_add_callback(fence, &bo->cb, qaic_fence_sync_cb_func);
	if (ret) {
		dma_fence_put(fence);
		bo->fence = NULL;
		return ret;
	}

	qaic_replace_bo_fence(bo, fence);

	return 0;
}
