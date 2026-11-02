// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Etnaviv Project
 */

#include <linux/moduleparam.h>

#include "etnaviv_drv.h"
#include "etnaviv_dump.h"
#include "etnaviv_gem.h"
#include "etnaviv_gpu.h"
#include "etnaviv_sched.h"
#include "state.xml.h"
#include "state_hi.xml.h"

static int etnaviv_job_hang_limit = 0;
module_param_named(job_hang_limit, etnaviv_job_hang_limit, int , 0444);
static int etnaviv_hw_jobs_limit = 4;
module_param_named(hw_job_limit, etnaviv_hw_jobs_limit, int , 0444);
static unsigned int etnaviv_hang_max_ms = 30000;
module_param_named(hang_max_ms, etnaviv_hang_max_ms, uint, 0644);
MODULE_PARM_DESC(hang_max_ms, "Time a busy job may run without front-end progress (ms, 0 = no extension)");

static struct dma_fence *etnaviv_sched_run_job(struct drm_sched_job *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);
	struct dma_fence *fence = NULL;

	if (likely(!sched_job->s_fence->finished.error))
		fence = etnaviv_gpu_submit(submit);
	else
		dev_dbg(submit->gpu->dev, "skipping bad job\n");

	return fence;
}

static enum drm_gpu_sched_stat etnaviv_sched_timedout_job(struct drm_sched_job
							  *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);
	struct etnaviv_gpu *gpu = submit->gpu;
	u32 dma_addr, primid = 0, mask;
	bool idle, was_idle;
	int change;

	/*
	 * If the GPU managed to complete this jobs fence, the timeout has
	 * fired before free-job worker. The timeout is spurious, so bail out.
	 */
	if (dma_fence_is_signaled(submit->out_fence))
		return DRM_GPU_SCHED_STAT_NO_HANG;

	/*
	 * If the GPU is still making forward progress on the front-end (which
	 * should never loop) we shift out the timeout to give it a chance to
	 * finish the job.
	 */
	dma_addr = gpu_read(gpu, VIVS_FE_DMA_ADDRESS);
	change = dma_addr - gpu->hangcheck_dma_addr;
	if (submit->exec_state == ETNA_PIPE_3D) {
		/* guard against concurrent usage from perfmon_sample */
		mutex_lock(&gpu->lock);
		gpu_write(gpu, VIVS_MC_PROFILE_CONFIG0,
			  VIVS_MC_PROFILE_CONFIG0_FE_CURRENT_PRIM <<
			  VIVS_MC_PROFILE_CONFIG0_FE__SHIFT);
		primid = gpu_read(gpu, VIVS_MC_PROFILE_FE_READ);
		mutex_unlock(&gpu->lock);
	}

	mask = gpu->idle_mask & ~(VIVS_HI_IDLE_STATE_FE | VIVS_HI_IDLE_STATE_MC);
	idle = (gpu_read(gpu, VIVS_HI_IDLE_STATE) & mask) == mask;
	was_idle = gpu->hangcheck_idle;
	gpu->hangcheck_idle = idle;

	if (gpu->state == ETNA_GPU_STATE_RUNNING &&
	    (gpu->completed_fence != gpu->hangcheck_fence ||
	     change < 0 || change > 16 ||
	     (submit->exec_state == ETNA_PIPE_3D &&
	      gpu->hangcheck_primid != primid))) {
		gpu->hangcheck_dma_addr = dma_addr;
		gpu->hangcheck_primid = primid;
		gpu->hangcheck_fence = gpu->completed_fence;
		gpu->hangcheck_busy = false;
		return DRM_GPU_SCHED_STAT_NO_HANG;
	}

	/*
	 * A compute dispatch is a single FE command, so the FE does not move
	 * while a long kernel runs. Keep the job alive as long as some unit
	 * other than FE and MC is busy, up to hang_max_ms. All of them idle at
	 * two ticks in a row means the FE waits for something that never
	 * comes.
	 */
	if (gpu->state == ETNA_GPU_STATE_RUNNING && !(idle && was_idle)) {
		unsigned long max = msecs_to_jiffies(READ_ONCE(etnaviv_hang_max_ms));

		if (!gpu->hangcheck_busy) {
			gpu->hangcheck_busy = true;
			gpu->hangcheck_busy_since = jiffies;
		}

		if (time_before(jiffies, gpu->hangcheck_busy_since + max))
			return DRM_GPU_SCHED_STAT_NO_HANG;

		if (max)
			dev_err(gpu->dev, "job busy for more than %u ms without FE progress\n",
				jiffies_to_msecs(max));
	}

	gpu->hangcheck_idle = false;
	gpu->hangcheck_busy = false;

	/* block scheduler */
	drm_sched_stop(&gpu->sched, sched_job);

	if(sched_job)
		drm_sched_increase_karma(sched_job);

	/* get the GPU back into the init state */
	etnaviv_core_dump(submit);
	etnaviv_gpu_recover_hang(submit);

	drm_sched_resubmit_jobs(&gpu->sched);

	drm_sched_start(&gpu->sched, 0);
	return DRM_GPU_SCHED_STAT_RESET;
}

static void etnaviv_sched_free_job(struct drm_sched_job *sched_job)
{
	struct etnaviv_gem_submit *submit = to_etnaviv_submit(sched_job);

	drm_sched_job_cleanup(sched_job);

	etnaviv_submit_put(submit);
}

static const struct drm_sched_backend_ops etnaviv_sched_ops = {
	.run_job = etnaviv_sched_run_job,
	.timedout_job = etnaviv_sched_timedout_job,
	.free_job = etnaviv_sched_free_job,
};

int etnaviv_sched_push_job(struct etnaviv_gem_submit *submit)
{
	struct etnaviv_gpu *gpu = submit->gpu;
	int ret;

	/*
	 * Hold the sched lock across the whole operation to avoid jobs being
	 * pushed out of order with regard to their sched fence seqnos as
	 * allocated in drm_sched_job_arm.
	 */
	mutex_lock(&gpu->sched_lock);

	ret = xa_alloc_cyclic(&gpu->user_fences, &submit->out_fence_id,
			      NULL, xa_limit_32b, &gpu->next_user_fence,
			      GFP_KERNEL);
	if (ret < 0)
		goto out_unlock;

	drm_sched_job_arm(&submit->sched_job);

	submit->out_fence = dma_fence_get(&submit->sched_job.s_fence->finished);

	xa_store(&gpu->user_fences, submit->out_fence_id,
		 submit->out_fence, GFP_KERNEL);

	/* the scheduler holds on to the job now */
	kref_get(&submit->refcount);

	drm_sched_entity_push_job(&submit->sched_job);

out_unlock:
	mutex_unlock(&gpu->sched_lock);

	return ret;
}

int etnaviv_sched_init(struct etnaviv_gpu *gpu)
{
	const struct drm_sched_init_args args = {
		.ops = &etnaviv_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = etnaviv_hw_jobs_limit,
		.hang_limit = etnaviv_job_hang_limit,
		.timeout = msecs_to_jiffies(500),
		.name = dev_name(gpu->dev),
		.dev = gpu->dev,
	};

	return drm_sched_init(&gpu->sched, &args);
}

void etnaviv_sched_fini(struct etnaviv_gpu *gpu)
{
	drm_sched_fini(&gpu->sched);
}
