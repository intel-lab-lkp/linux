// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/gpu_scheduler.h>
#include <linux/dma-fence.h>
#include <linux/workqueue.h>

#include "qaic.h"

#define QAIC_CREDITS SZ_64K

static void qaic_job_fini(struct drm_sched_job *sched_job)
{
	struct qaic_job *job = to_qaic_job(sched_job);

	qaic_cleanup_job(job);
}

static struct dma_fence *qaic_job_run(struct drm_sched_job *sched_job)
{
	struct qaic_job *job = to_qaic_job(sched_job);
	struct dma_bridge_chan *dbc = job->dbc;
	struct bo_slice *slice = job->slice;
	struct dma_fence *fence;
	int ret;

	fence = dma_fence_get(&job->irq_fence->base);

	/* Job has been cancelled */
	if (fence->error) {
		ret = fence->error;
		goto exit;
	}

	ret = mutex_lock_interruptible(&dbc->req_lock);
	if (ret)
		goto exit;

	ret = qaic_submit_reqs_to_hw(slice, job->num_req, job->partial,
				     job->partial_size, job->id);
	if (ret)
		goto unlock_dbc;
	mutex_unlock(&dbc->req_lock);

	return fence;

unlock_dbc:
	mutex_unlock(&dbc->req_lock);
exit:
	dma_fence_put(fence);
	return ERR_PTR(ret);
}

static const struct drm_sched_backend_ops qaic_sched_ops = {
	.run_job = qaic_job_run,
	.free_job = qaic_job_fini,
};

static void qaicm_sched_fini(struct drm_device *dev, void *res)
{
	struct dma_bridge_chan *dbc = res;

	drm_sched_fini(&dbc->sched);
	destroy_workqueue(dbc->submit_wq);
}

/*
 * Currently, drm_sched is configured to always have credits equal to 64k, but
 * the network can configure different HW queue sizes. Create a scaling ratio to
 * convert sizes of HW queue to size in credits.
 *
 * The ratio expresses the cost of each queue element. For example, if nelem=10,
 * then the ratio is 6554 credits consumed per queue element. This cost,
 * together with the queue credit balance, limits the number of items in the
 * queue at any time.
 *
 * The credit limit should be greater than the typical HW queue so that the
 * scaling ratio is never less than 1. 'nelem' is bounds checked in
 * encode_activate() to guarantee that it is less than QAIC_CREDITS. Round up
 * the scaling ratio if it isn't perfectly divisible.
 */
void set_dbc_scaling_ratio(struct dma_bridge_chan *dbc, u32 nelem)
{
	if (!nelem) {
		/*
		 * There is a chance of enable_dbc() being called after a failed deactivation of the
		 * dbc. In that case, set the credit ratio to a value that shouldn't let any further
		 * work be scheduled.
		 */
		dbc->credit_ratio = QAIC_CREDITS;
		return;
	}
	if (QAIC_CREDITS % nelem != 0)
		pr_debug("Credits not evenly divisible by queue size: size = %d\n", nelem);
	dbc->credit_ratio = DIV_ROUND_UP(QAIC_CREDITS, nelem);
}

int qaicm_sched_init(struct drm_device *drm, struct dma_bridge_chan *dbc)
{
	int ret;

	/*
	 * Using out own workqueue here due to performance regression with
	 * large numbers of devices and jobs (32+ and 16+ respectively).
	 *
	 * Workqueues with WQ_UNBOUND + max_active > 1 also prevented this
	 * regression, but introduced the possibility of unordered
	 * submission to the HW queue. Under testing that did not seem to
	 * cause any issues, though eventually decided to go with this to
	 * minimize changes to existing HW interaction patterns.
	 */
	dbc->submit_wq = alloc_workqueue("%s", WQ_MEM_RECLAIM | WQ_PERCPU, 1, dbc->name);
	if (!dbc->submit_wq)
		return -EINVAL;

	const struct drm_sched_init_args args = {
		.ops = &qaic_sched_ops,
		.submit_wq = dbc->submit_wq,
		.num_rqs = 1,
		.credit_limit = (QAIC_CREDITS - 1),
		.hang_limit = 1,
		.timeout = MAX_SCHEDULE_TIMEOUT,
		.name = dbc->name,
		.dev = drm->dev,
	};

	ret = drm_sched_init(&dbc->sched, &args);
	if (ret) {
		destroy_workqueue(dbc->submit_wq);
		return ret;
	}

	return drmm_add_action_or_reset(drm, qaicm_sched_fini, dbc);
}

void qaic_sched_entity_init(struct dma_bridge_chan *dbc)
{
	struct drm_gpu_scheduler *sched = &dbc->sched;

	drm_sched_entity_init(&dbc->sched_entity, DRM_SCHED_PRIORITY_KERNEL, &sched, 1, NULL);
}

struct qaic_job *qaic_create_job(struct drm_file *file_priv, struct bo_slice *slice,
				 unsigned int num_req, u64 seq_no, struct list_head *tmp_list)
{
	struct dma_bridge_chan *dbc = slice->bo->dbc;
	struct qaic_bo *bo = slice->bo;
	struct qaic_job *job;
	u32 credits;
	u16 req_id;
	int ret, i;

	if (check_mul_overflow((u32)num_req, dbc->credit_ratio, &credits))
		return ERR_PTR(-EINVAL);

	job = kzalloc_obj(*job);
	if (!job)
		return ERR_PTR(-ENOMEM);

	req_id = (u16) atomic_inc_return(&dbc->next_req_id);

	job->irq_fence = qaic_create_irq_fence(bo, seq_no, req_id);
	if (IS_ERR(job->irq_fence)) {
		ret = PTR_ERR(job->irq_fence);
		goto free_job;
	}

	ret = drm_sched_job_init(&job->base, &dbc->sched_entity, credits,
				 dbc, file_priv->client_id);
	if (ret)
		goto put_fence;

	/*
	 * Make new job depend on the pre-existing fences in BO reservation.
	 * DRM scheduler will wait on every fence in the dependency list
	 * before running this job.
	 */
	ret = drm_sched_job_add_implicit_dependencies(&job->base, &bo->base, true);
	if (ret)
		goto job_cleanup;

	job->slice = slice;
	job->num_req = num_req;
	job->dbc = dbc;
	INIT_LIST_HEAD(&job->queue);
	job->id = req_id;
	kref_init(&job->ref_count);
	for (i = 0; i < job->num_req; i++)
		slice->reqs[i].req_id = cpu_to_le16(job->id);

	list_add_tail(&job->queue, tmp_list);
	drm_gem_object_get(&bo->base);
	job->bo = bo;
	return job;

job_cleanup:
	drm_sched_job_cleanup(&job->base);
put_fence:
	dma_fence_put(&job->irq_fence->base);
free_job:
	kfree(job);
	return ERR_PTR(ret);
}

void qaic_submit_job(struct dma_bridge_chan *dbc, struct qaic_job *job)
{
	kref_get(&job->ref_count);
	drm_sched_job_arm(&job->base);
	drm_sched_entity_push_job(&job->base);
	dbc->pending_reqs += job->num_req;
}

void qaic_free_job(struct kref *ref)
{
	struct qaic_job *job = container_of(ref, struct qaic_job, ref_count);

	dma_fence_put(&job->irq_fence->base);
	drm_gem_object_put(&job->bo->base);
	kfree(job);
}

void qaic_put_job(struct qaic_job *job)
{
	kref_put(&job->ref_count, qaic_free_job);
}

void qaic_cleanup_job(struct qaic_job *job)
{
	drm_sched_job_cleanup(&job->base);
	qaic_put_job(job);
}
