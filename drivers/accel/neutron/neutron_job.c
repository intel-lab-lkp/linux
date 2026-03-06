// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2025-2026 NXP */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/neutron_accel.h>

#include "neutron_driver.h"
#include "neutron_device.h"
#include "neutron_gem.h"
#include "neutron_mailbox.h"
#include "neutron_job.h"

#define NEUTRON_JOB_TIMEOUT_MS 5000

static const char *neutron_fence_get_driver_name(struct dma_fence *fence)
{
	return "neutron";
}

static const char *neutron_fence_get_timeline_name(struct dma_fence *fence)
{
	return "neutron-npu";
}

static const struct dma_fence_ops neutron_fence_ops = {
	.get_driver_name = neutron_fence_get_driver_name,
	.get_timeline_name = neutron_fence_get_timeline_name,
};

static void neutron_hw_submit(struct neutron_job *job)
{
	struct neutron_device *ndev = job->ndev;
	struct neutron_mbox_cmd cmd = {0};
	u32 base_l, base_h;
	u64 base_addr;
	int ret;

	switch (job->type) {
	case DRM_NEUTRON_JOB_INFERENCE:
		cmd.id = NEUTRON_CMD_INFERENCE;
		cmd.args[0] = job->inference.tensor_offset;
		cmd.args[1] = job->inference.microcode_offset;
		cmd.args[2] = job->inference.tensor_count;
		break;
	default:
		dev_WARN(ndev->dev, "Unknown job type: %d\n", job->type);
		return;
	}

	base_addr = to_drm_gem_dma_obj(job->bo)->dma_addr;
	base_l = lower_32_bits(base_addr);
	base_h = upper_32_bits(base_addr);

	writel_relaxed(base_l, NEUTRON_REG(ndev, BASEDDRL));
	writel_relaxed(base_l, NEUTRON_REG(ndev, BASEINOUTL));
	writel_relaxed(base_l, NEUTRON_REG(ndev, BASESPILLL));
	writel_relaxed(base_h, NEUTRON_REG(ndev, BASEDDRH));
	writel_relaxed(base_h, NEUTRON_REG(ndev, BASEINOUTH));
	writel_relaxed(base_h, NEUTRON_REG(ndev, BASESPILLH));

	ret = neutron_mbox_send_cmd(ndev, &cmd);
	if (ret) {
		/* Nothing we can do here, we'll reset the device on timeout */
		dev_err(ndev->dev, "Failed to submit job, device is busy\n");
	}
}

void neutron_job_err_handler(struct neutron_device *ndev)
{
	guard(mutex)(&ndev->job_lock);

	if (ndev->active_job)
		drm_sched_fault(&ndev->sched);
}

void neutron_job_done_handler(struct neutron_device *ndev)
{
	struct neutron_mbox_state state;
	struct dma_fence *fence;

	neutron_mbox_read_state(ndev, &state);
	if (state.status != NEUTRON_FW_STATUS_DONE) {
		dev_err(ndev->dev, "Inconsistent firmware state: status 0x%x, err 0x%x\n",
			state.status, state.err_code);
		return neutron_job_err_handler(ndev);
	}

	/* Reset Neutron internal state to prepare for next inference */
	neutron_mbox_reset_state(ndev);

	scoped_guard(mutex, &ndev->job_lock) {
		if (ndev->active_job) {
			fence = ndev->active_job->neutron_fence;
			if (state.err_code != 0) {
				dev_warn(ndev->dev, "Job finished with error: 0x%x\n",
					 state.err_code);
				dma_fence_set_error(fence, state.err_code);
			}
			dma_fence_signal(fence);
			ndev->active_job = NULL;
		}
	}
}

static void neutron_cleanup_job(struct kref *ref)
{
	struct neutron_job *job = container_of(ref, struct neutron_job, refcnt);

	pm_runtime_put_autosuspend(job->ndev->base.dev);

	dma_fence_put(job->neutron_fence);
	drm_gem_object_put(job->bo);

	kfree(job);
}

static void neutron_put_job(struct neutron_job *job)
{
	kref_put(&job->refcnt, neutron_cleanup_job);
}

static void neutron_free_job(struct drm_sched_job *sched_job)
{
	struct neutron_job *job = to_neutron_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	neutron_put_job(job);
}

static struct dma_fence *neutron_run_job(struct drm_sched_job *sched_job)
{
	struct neutron_job *job = to_neutron_job(sched_job);
	struct dma_fence *fence = job->neutron_fence;
	struct neutron_device *ndev = job->ndev;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	dma_fence_init(fence, &neutron_fence_ops, NULL,
		       ndev->fence_context, ++ndev->job_seqno);
	dma_fence_get(fence);

	scoped_guard(mutex, &ndev->job_lock) {
		ndev->active_job = job;
		neutron_hw_submit(job);
	}

	return fence;
}

static enum drm_gpu_sched_stat neutron_timedout_job(struct drm_sched_job *sched_job)
{
	struct neutron_job *job = to_neutron_job(sched_job);
	struct neutron_device *ndev = job->ndev;
	struct neutron_mbox_state state;

	/* We assume Neutron is stuck, retrieve current state and reset */
	neutron_mbox_read_state(ndev, &state);
	dev_err(ndev->dev, "Neutron timedout, status: 0x%x, err: 0x%x\n",
		state.status, state.err_code);

	drm_sched_stop(&ndev->sched, sched_job);

	scoped_guard(mutex, &ndev->job_lock)
		ndev->active_job = NULL;

	pm_runtime_force_suspend(ndev->dev);
	pm_runtime_force_resume(ndev->dev);

	drm_sched_start(&ndev->sched, 0);

	return DRM_GPU_SCHED_STAT_RESET;
}

static void neutron_cancel_job(struct drm_sched_job *sched_job)
{
	struct neutron_job *job = to_neutron_job(sched_job);
	struct neutron_device *ndev = job->ndev;

	guard(mutex)(&ndev->job_lock);

	if (!dma_fence_is_signaled(job->neutron_fence)) {
		dma_fence_set_error(job->neutron_fence, -ECANCELED);
		dma_fence_signal(job->neutron_fence);
	}
}

static const struct drm_sched_backend_ops neutron_sched_ops = {
	.run_job = neutron_run_job,
	.free_job = neutron_free_job,
	.timedout_job = neutron_timedout_job,
	.cancel_job = neutron_cancel_job,
};

int neutron_job_init(struct neutron_device *ndev)
{
	const struct drm_sched_init_args args = {
		.ops = &neutron_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(NEUTRON_JOB_TIMEOUT_MS),
		.name = dev_name(ndev->dev),
		.dev = ndev->dev,
	};
	int ret;

	ret = devm_mutex_init(ndev->dev, &ndev->sched_lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(ndev->dev, &ndev->job_lock);
	if (ret)
		return ret;

	ndev->fence_context = dma_fence_context_alloc(1);

	ret = drm_sched_init(&ndev->sched, &args);
	if (ret)
		dev_err(ndev->dev, "Error creating DRM scheduler\n");

	return ret;
}

void neutron_job_fini(struct neutron_device *ndev)
{
	drm_sched_fini(&ndev->sched);
}

int neutron_job_open(struct neutron_file_priv *npriv)
{
	struct neutron_device *ndev = npriv->ndev;
	struct drm_gpu_scheduler *sched = &ndev->sched;
	int ret;

	ret = drm_sched_entity_init(&npriv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL,
				    &sched, 1, NULL);
	if (ret)
		dev_err(ndev->dev, "Error creating scheduler entity\n");

	return ret;
}

void neutron_job_close(struct neutron_file_priv *npriv)
{
	drm_sched_entity_destroy(&npriv->sched_entity);
}

static int neutron_push_job(struct neutron_job *job, struct drm_syncobj *sync)
{
	struct neutron_device *ndev = job->ndev;
	struct ww_acquire_ctx acquire_ctx;
	struct dma_fence *sched_fence;
	int ret;

	ret = drm_gem_lock_reservations(&job->bo, 1, &acquire_ctx);
	if (ret)
		return ret;

	ret = dma_resv_reserve_fences(job->bo->resv, 1);
	if (ret)
		goto out_unlock_res;

	ret = drm_sched_job_add_implicit_dependencies(&job->base, job->bo, true);
	if (ret)
		goto out_unlock_res;

	ret = pm_runtime_resume_and_get(ndev->base.dev);
	if (ret)
		goto out_unlock_res;

	scoped_guard(mutex, &ndev->sched_lock) {
		drm_sched_job_arm(&job->base);

		sched_fence = dma_fence_get(&job->base.s_fence->finished);
		drm_syncobj_replace_fence(sync, sched_fence);

		kref_get(&job->refcnt);
		drm_sched_entity_push_job(&job->base);

		dma_resv_add_fence(job->bo->resv, sched_fence,
				   DMA_RESV_USAGE_WRITE);

		dma_fence_put(sched_fence);
	}

out_unlock_res:
	drm_gem_unlock_reservations(&job->bo, 1, &acquire_ctx);

	return ret;
}

int neutron_ioctl_submit_job(struct drm_device *drm, void *data, struct drm_file *filp)
{
	struct neutron_device *ndev = to_neutron_device(drm);
	struct neutron_file_priv *npriv = filp->driver_priv;
	struct drm_neutron_submit_job *args = data;
	struct drm_syncobj *syncobj;
	struct neutron_job *job;
	int ret;

	if (args->pad)
		return -EINVAL;

	job = kzalloc_obj(*job);
	if (!job)
		return -ENOMEM;

	job->ndev = ndev;
	kref_init(&job->refcnt);

	job->neutron_fence = kzalloc_obj(*job->neutron_fence);
	if (!job->neutron_fence) {
		ret = -ENOMEM;
		goto out_free_job;
	}

	switch (args->type) {
	case DRM_NEUTRON_JOB_INFERENCE:
		memcpy(&job->inference, &args->inference,
		       sizeof(args->inference));
		break;
	default:
		dev_dbg(ndev->dev, "Invalid job type %d\n", args->type);
		ret = -EINVAL;
		goto out_free_fence;
	}

	job->bo = drm_gem_object_lookup(filp, args->bo_handle);
	if (!job->bo) {
		dev_dbg(ndev->dev, "Invalid BO handle\n");
		ret = -ENOENT;
		goto out_free_fence;
	}

	syncobj = drm_syncobj_find(filp, args->syncobj_handle);
	if (!syncobj) {
		dev_dbg(ndev->dev, "Invalid syncobj handle\n");
		ret = -ENOENT;
		goto out_put_gem;
	}

	ret = drm_sched_job_init(&job->base, &npriv->sched_entity, 1, NULL,
				 filp->client_id);
	if (ret)
		goto out_put_syncobj;

	ret = neutron_push_job(job, syncobj);
	if (ret)
		goto out_sched_cleanup;

	neutron_put_job(job);
	drm_syncobj_put(syncobj);

	return 0;

out_sched_cleanup:
	drm_sched_job_cleanup(&job->base);
out_put_syncobj:
	drm_syncobj_put(syncobj);
out_put_gem:
	drm_gem_object_put(job->bo);
out_free_fence:
	kfree(job->neutron_fence);
out_free_job:
	kfree(job);

	return ret;
}
