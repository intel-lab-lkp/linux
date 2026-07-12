// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Raspberry Pi */

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_print.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc4_trace.h"

static struct vc4_job *
to_vc4_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct vc4_job, base);
}

static struct vc4_bin_job *
to_bin_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct vc4_bin_job, base.base);
}

static struct vc4_render_job *
to_render_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct vc4_render_job, base.base);
}

static void
vc4_flush_caches(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Flush the GPU L2 caches.  These caches sit on top of system
	 * L3 (the 128kb or so shared with the CPU), and are
	 * non-allocating in the L3.
	 */
	V3D_WRITE(V3D_L2CACTL, V3D_L2CACTL_L2CCLR);

	V3D_WRITE(V3D_SLCACTL,
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T1CC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T0CC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_UCC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_ICC));
}

static void
vc4_flush_texture_caches(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	V3D_WRITE(V3D_L2CACTL, V3D_L2CACTL_L2CCLR);

	V3D_WRITE(V3D_SLCACTL,
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T1CC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T0CC));
}

static void
vc4_sched_job_free(struct drm_sched_job *sched_job)
{
	struct vc4_job *job = to_vc4_job(sched_job);

	vc4_job_cleanup(job);
}

static struct dma_fence *vc4_bin_job_run(struct drm_sched_job *sched_job)
{
	struct vc4_bin_job *job = to_bin_job(sched_job);
	struct vc4_dev *vc4 = job->base.vc4;
	struct drm_device *dev = &vc4->base;
	struct dma_fence *fence = NULL;

	if (unlikely(job->base.base.s_fence->finished.error))
		goto out_clean_job;

	/* Lock required around bin_job update vs vc4_overflow_mem_work(). */
	spin_lock_irq(&vc4->job_lock);
	vc4->bin_job = job;

	/* Reset the overflow allocation registers to prevent the binner from
	 * writing to a slot that a concurrent render job might free. The first
	 * OUTOMEM interrupt will allocate a fresh slot.
	 */
	V3D_WRITE(V3D_BPOA, 0);
	V3D_WRITE(V3D_BPOS, 0);
	spin_unlock_irq(&vc4->job_lock);

	vc4_flush_caches(dev);

	fence = vc4_fence_create(vc4, VC4_BIN);
	if (IS_ERR(fence))
		goto out_clean_job;

	/* Release the previous fence if the scheduler is resubmitting this
	 * job after a GPU reset.
	 */
	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_vc4_submit_cl(dev, false, to_vc4_fence(fence)->seqno,
			    job->ct0ca, job->ct0ea);

	vc4_perfmon_start(vc4, job->base.perfmon);

	V3D_WRITE(V3D_CTNCA(0), job->ct0ca);
	V3D_WRITE(V3D_CTNEA(0), job->ct0ea);

	return fence;

out_clean_job:
	spin_lock_irq(&vc4->job_lock);
	vc4->bin_job = NULL;
	spin_unlock_irq(&vc4->job_lock);
	return fence;
}

static struct dma_fence *vc4_render_job_run(struct drm_sched_job *sched_job)
{
	struct vc4_render_job *job = to_render_job(sched_job);
	struct vc4_dev *vc4 = job->base.vc4;
	struct drm_device *dev = &vc4->base;
	struct dma_fence *fence = NULL;

	if (unlikely(job->base.base.s_fence->finished.error))
		goto out_clean_job;

	spin_lock_irq(&vc4->job_lock);
	vc4->render_job = job;
	spin_unlock_irq(&vc4->job_lock);

	/* A previous RCL may have written to one of our textures, and
	 * our full cache flush at bin time may have occurred before
	 * that RCL completed. Flush the texture cache now, but not
	 * the instructions or uniforms (since we don't write those
	 * from an RCL).
	 */
	vc4_flush_texture_caches(dev);

	fence = vc4_fence_create(vc4, VC4_RENDER);
	if (IS_ERR(fence))
		goto out_clean_job;

	/* Release the previous fence if the scheduler is resubmitting this
	 * job after a GPU reset.
	 */
	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_vc4_submit_cl(dev, true, to_vc4_fence(fence)->seqno,
			    job->ct1ca, job->ct1ea);

	vc4_perfmon_start(vc4, job->base.perfmon);

	V3D_WRITE(V3D_CTNCA(1), job->ct1ca);
	V3D_WRITE(V3D_CTNEA(1), job->ct1ea);

	return fence;

out_clean_job:
	spin_lock_irq(&vc4->job_lock);
	vc4->render_job = NULL;
	spin_unlock_irq(&vc4->job_lock);

	return fence;
}

static void
vc4_reset(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	drm_err(dev, "Resetting GPU.\n");

	mutex_lock(&vc4->power_lock);
	if (vc4->power_refcount) {
		/* Power the device off and back on the by dropping the
		 * reference on runtime PM.
		 */
		pm_runtime_put_sync_suspend(&vc4->v3d->pdev->dev);
		pm_runtime_get_sync(&vc4->v3d->pdev->dev);
	}
	mutex_unlock(&vc4->power_lock);

	vc4_irq_reset(dev);
}

static enum drm_gpu_sched_stat
vc4_gpu_reset_for_timeout(struct vc4_dev *vc4, struct drm_sched_job *sched_job)
{
	struct vc4_job *job = to_vc4_job(sched_job);
	enum vc4_queue q;

	mutex_lock(&vc4->reset_lock);

	/* block scheduler */
	for (q = 0; q < VC4_MAX_QUEUES; q++)
		drm_sched_stop(&vc4->queue[q].sched, sched_job);

	if (sched_job) {
		drm_sched_increase_karma(sched_job);

		/* If the guilty job is a BIN job, also increase the karma
		 * of its paired render job. Otherwise, the RENDER job would
		 * be submitted to the GPU without binner output.
		 */
		if (sched_job->sched == &vc4->queue[VC4_BIN].sched) {
			struct vc4_bin_job *bin = to_bin_job(sched_job);

			drm_sched_increase_karma(&bin->render->base.base);
		}
	}

	vc4_perfmon_stop(vc4, job->perfmon, false);
	vc4_save_hang_state(&vc4->base);

	/* get the GPU back into the init state */
	vc4_reset(&vc4->base);

	for (q = 0; q < VC4_MAX_QUEUES; q++)
		drm_sched_resubmit_jobs(&vc4->queue[q].sched);

	/* Unblock schedulers and restart their jobs. */
	for (q = 0; q < VC4_MAX_QUEUES; q++)
		drm_sched_start(&vc4->queue[q].sched, 0);

	mutex_unlock(&vc4->reset_lock);

	return DRM_GPU_SCHED_STAT_RESET;
}

static enum drm_gpu_sched_stat
vc4_cl_job_timedout(struct drm_sched_job *sched_job, enum vc4_queue q)
{
	struct vc4_job *job = to_vc4_job(sched_job);
	struct vc4_dev *vc4 = job->vc4;
	u32 ctca = V3D_READ(V3D_CTNCA(q));
	u32 ctra = V3D_READ(V3D_CTNRA0(q));

	/* If the current address or return address have changed, then the GPU
	 * has probably made progress and we should delay the reset. This could
	 * fail if the GPU got in an infinite loop in the CL, but that is pretty
	 * unlikely outside of an i-g-t testcase.
	 */
	if (job->timedout_ctca != ctca || job->timedout_ctra != ctra) {
		job->timedout_ctca = ctca;
		job->timedout_ctra = ctra;

		return DRM_GPU_SCHED_STAT_NO_HANG;
	}

	return vc4_gpu_reset_for_timeout(vc4, sched_job);
}

static enum drm_gpu_sched_stat
vc4_bin_job_timedout(struct drm_sched_job *sched_job)
{
	return vc4_cl_job_timedout(sched_job, VC4_BIN);
}

static enum drm_gpu_sched_stat
vc4_render_job_timedout(struct drm_sched_job *sched_job)
{
	return vc4_cl_job_timedout(sched_job, VC4_RENDER);
}

static const struct drm_sched_backend_ops vc4_bin_sched_ops = {
	.run_job = vc4_bin_job_run,
	.timedout_job = vc4_bin_job_timedout,
	.free_job = vc4_sched_job_free,
};

static const struct drm_sched_backend_ops vc4_render_sched_ops = {
	.run_job = vc4_render_job_run,
	.timedout_job = vc4_render_job_timedout,
	.free_job = vc4_sched_job_free,
};

static int
vc4_queue_sched_init(struct vc4_dev *vc4, const struct drm_sched_backend_ops *ops,
		     enum vc4_queue q, const char *name)
{
	struct vc4_queue_state *queue = &vc4->queue[q];
	struct drm_sched_init_args args = {
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(500),
		.dev = vc4->base.dev,
	};

	args.ops = ops;
	args.name = name;

	queue->fence_context = dma_fence_context_alloc(1);

	return drm_sched_init(&queue->sched, &args);
}

int
vc4_sched_init(struct vc4_dev *vc4)
{
	int ret;

	ret = vc4_queue_sched_init(vc4, &vc4_bin_sched_ops,
				   VC4_BIN, "vc4_bin");
	if (ret)
		return ret;

	ret = vc4_queue_sched_init(vc4, &vc4_render_sched_ops,
				   VC4_RENDER, "vc4_render");
	if (ret) {
		vc4_sched_fini(vc4);
		return ret;
	}

	return 0;
}

void
vc4_sched_fini(struct vc4_dev *vc4)
{
	enum vc4_queue q;

	for (q = 0; q < VC4_MAX_QUEUES; q++) {
		if (vc4->queue[q].sched.ready)
			drm_sched_fini(&vc4->queue[q].sched);
	}
}
