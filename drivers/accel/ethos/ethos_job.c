// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */
/* Copyright 2025 Arm, Ltd. */

#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/ethos_accel.h>

#include "ethos_device.h"
#include "ethos_drv.h"
#include "ethos_gem.h"
#include "ethos_job.h"

#define JOB_TIMEOUT_MS 500

static struct ethos_job *to_ethos_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct ethos_job, base);
}

static const char *ethos_fence_get_driver_name(struct dma_fence *fence)
{
	return "ethos";
}

static const char *ethos_fence_get_timeline_name(struct dma_fence *fence)
{
	return "ethos-npu";
}

static const struct dma_fence_ops ethos_fence_ops = {
	.get_driver_name = ethos_fence_get_driver_name,
	.get_timeline_name = ethos_fence_get_timeline_name,
};

static struct dma_fence *ethos_fence_create(struct ethos_device *dev)
{
	struct dma_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	dma_fence_init(fence, &ethos_fence_ops, &dev->fence_lock,
		       dev->fence_context, ++dev->emit_seqno);

	return fence;
}

static void ethos_job_hw_submit(struct ethos_device *dev, struct ethos_job *job)
{
	struct drm_gem_dma_object *cmd_bo = to_drm_gem_dma_obj(job->cmd_bo);
	struct ethos_validated_cmdstream_info *cmd_info = to_ethos_bo(job->cmd_bo)->info;

	/* Don't queue the job if a reset is in progress */
	if (atomic_read(&dev->reset.pending))
		return;

	for (int i = 0; i < job->region_cnt; i++) {
		struct drm_gem_dma_object *bo;
		int region = job->region_bo_num[i];

		bo = to_drm_gem_dma_obj(job->region_bo[i]);
		writel_relaxed(lower_32_bits(bo->dma_addr), dev->regs + NPU_REG_BASEP(region));
		writel_relaxed(upper_32_bits(bo->dma_addr), dev->regs + NPU_REG_BASEP_HI(region));
		dev_dbg(dev->base.dev, "Region %d base addr = %pad\n", region, &bo->dma_addr);
	}

	if (job->sram_size) {
		writel_relaxed(lower_32_bits(dev->sramphys),
			       dev->regs + NPU_REG_BASEP(ETHOS_SRAM_REGION));
		writel_relaxed(upper_32_bits(dev->sramphys),
			       dev->regs + NPU_REG_BASEP_HI(ETHOS_SRAM_REGION));
		dev_dbg(dev->base.dev, "Region %d base addr = %pad (SRAM)\n",
			 ETHOS_SRAM_REGION, &dev->sramphys);
	}

	writel_relaxed(lower_32_bits(cmd_bo->dma_addr), dev->regs + NPU_REG_QBASE);
	writel_relaxed(upper_32_bits(cmd_bo->dma_addr), dev->regs + NPU_REG_QBASE_HI);
	writel_relaxed(cmd_info->cmd_size, dev->regs + NPU_REG_QSIZE);

	writel(CMD_TRANSITION_TO_RUN, dev->regs + NPU_REG_CMD);

	dev_dbg(dev->base.dev,
		"Submitted cmd at %pad to core\n", &cmd_bo->dma_addr);
}

static int ethos_acquire_object_fences(struct ethos_job *job)
{
	int i, ret;
	struct drm_gem_object **bos = job->region_bo;
	struct ethos_validated_cmdstream_info *info = to_ethos_bo(job->cmd_bo)->info;

	for (i = 0; i < job->region_cnt; i++) {
		bool is_write;

		if (!bos[i])
			break;

		ret = dma_resv_reserve_fences(bos[i]->resv, 1);
		if (ret)
			return ret;

		is_write = info->output_region[job->region_bo_num[i]];
		ret = drm_sched_job_add_implicit_dependencies(&job->base, bos[i],
							      is_write);
		if (ret)
			return ret;
	}

	return 0;
}

static void ethos_attach_object_fences(struct ethos_job *job)
{
	int i;
	struct dma_fence *fence = job->inference_done_fence;
	struct drm_gem_object **bos = job->region_bo;
	struct ethos_validated_cmdstream_info *info = to_ethos_bo(job->cmd_bo)->info;

	for (i = 0; i < job->region_cnt; i++)
		if (info->output_region[job->region_bo_num[i]])
			dma_resv_add_fence(bos[i]->resv, fence, DMA_RESV_USAGE_WRITE);
}

static int ethos_job_do_push(struct ethos_job *job)
{
	struct ethos_device *dev = job->dev;
	int ret;

	guard(mutex)(&dev->sched_lock);

	drm_sched_job_arm(&job->base);

	job->inference_done_fence = dma_fence_get(&job->base.s_fence->finished);

	ret = ethos_acquire_object_fences(job);
	if (ret)
		return ret;

	kref_get(&job->refcount); /* put by scheduler job completion */

	drm_sched_entity_push_job(&job->base);

	return 0;
}

static int ethos_job_push(struct ethos_job *job)
{
	struct ww_acquire_ctx acquire_ctx;
	int ret;

	ret = drm_gem_lock_reservations(job->region_bo, job->region_cnt, &acquire_ctx);
	if (ret)
		return ret;

	ret = ethos_job_do_push(job);
	if (!ret)
		ethos_attach_object_fences(job);

	drm_gem_unlock_reservations(job->region_bo, job->region_cnt, &acquire_ctx);
	return ret;
}

static void ethos_job_cleanup(struct kref *ref)
{
	struct ethos_job *job = container_of(ref, struct ethos_job,
						refcount);
	unsigned int i;

	dma_fence_put(job->done_fence);
	dma_fence_put(job->inference_done_fence);

	for (i = 0; i < job->region_cnt; i++)
		drm_gem_object_put(job->region_bo[i]);

	kfree(job);
}

static void ethos_job_put(struct ethos_job *job)
{
	kref_put(&job->refcount, ethos_job_cleanup);
}

static void ethos_job_free(struct drm_sched_job *sched_job)
{
	struct ethos_job *job = to_ethos_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	ethos_job_put(job);
}

static struct dma_fence *ethos_job_run(struct drm_sched_job *sched_job)
{
	struct ethos_job *job = to_ethos_job(sched_job);
	struct ethos_device *dev = job->dev;
	struct dma_fence *fence = NULL;
	int ret;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	fence = ethos_fence_create(dev);
	if (IS_ERR(fence))
		return fence;

	if (job->done_fence)
		dma_fence_put(job->done_fence);
	job->done_fence = dma_fence_get(fence);

	ret = pm_runtime_get_sync(dev->base.dev);
	if (ret < 0)
		return fence;

	scoped_guard(mutex, &dev->job_lock) {
		dev->in_flight_job = job;
		ethos_job_hw_submit(dev, job);
	}

	return fence;
}

static void ethos_job_handle_irq(struct ethos_device *dev)
{
	u32 status;

	pm_runtime_mark_last_busy(dev->base.dev);

	status = readl_relaxed(dev->regs + NPU_REG_STATUS);

	if (status & (STATUS_BUS_STATUS | STATUS_CMD_PARSE_ERR)) {
		dev_err(dev->base.dev, "Error IRQ - %x\n", status);
		drm_sched_fault(&dev->sched);
		return;
	}

	scoped_guard(mutex, &dev->job_lock) {
		if (dev->in_flight_job) {
			dma_fence_signal(dev->in_flight_job->done_fence);
			pm_runtime_put_autosuspend(dev->base.dev);
			dev->in_flight_job = NULL;
		}
	}
}

static irqreturn_t ethos_job_irq_handler_thread(int irq, void *data)
{
	struct ethos_device *dev = data;

	ethos_job_handle_irq(dev);

	return IRQ_HANDLED;
}

static irqreturn_t ethos_job_irq_handler(int irq, void *data)
{
	struct ethos_device *dev = data;
	u32 status = readl_relaxed(dev->regs + NPU_REG_STATUS);

	if (!(status & STATUS_IRQ_RAISED))
		return IRQ_NONE;

	writel_relaxed(CMD_CLEAR_IRQ, dev->regs + NPU_REG_CMD);
	return IRQ_WAKE_THREAD;
}

static void ethos_reset(struct ethos_device *dev, struct drm_sched_job *bad)
{
	if (!atomic_read(&dev->reset.pending))
		return;

	drm_sched_stop(&dev->sched, bad);

	/*
	 * Remaining interrupts have been handled, but we might still have
	 * stuck jobs. Let's make sure the PM counters stay balanced by
	 * manually calling pm_runtime_put_noidle().
	 */
	scoped_guard(mutex, &dev->job_lock) {
		if (dev->in_flight_job)
			pm_runtime_put_noidle(dev->base.dev);

		dev->in_flight_job = NULL;
	}

	/* Proceed with reset now. */
	pm_runtime_force_suspend(dev->base.dev);
	pm_runtime_force_resume(dev->base.dev);

	/* NPU has been reset, we can clear the reset pending bit. */
	atomic_set(&dev->reset.pending, 0);

	/* Restart the scheduler */
	drm_sched_start(&dev->sched, 0);
}

static enum drm_gpu_sched_stat ethos_job_timedout(struct drm_sched_job *sched_job)
{
	struct ethos_device *dev = to_ethos_job(sched_job)->dev;

	dev_err(dev->base.dev, "NPU sched timed out\n");

	atomic_set(&dev->reset.pending, 1);
	ethos_reset(dev, sched_job);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void ethos_reset_work(struct work_struct *work)
{
	struct ethos_device *dev;

	dev = container_of(work, struct ethos_device, reset.work);
	ethos_reset(dev, NULL);
}

static const struct drm_sched_backend_ops ethos_sched_ops = {
	.run_job = ethos_job_run,
	.timedout_job = ethos_job_timedout,
	.free_job = ethos_job_free
};

int ethos_job_init(struct ethos_device *dev)
{
	struct drm_sched_init_args args = {
		.ops = &ethos_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(JOB_TIMEOUT_MS),
		.name = dev_name(dev->base.dev),
		.dev = dev->base.dev,
	};
	int ret;

	INIT_WORK(&dev->reset.work, ethos_reset_work);
	spin_lock_init(&dev->fence_lock);
	mutex_init(&dev->job_lock);

	dev->irq = platform_get_irq(to_platform_device(dev->base.dev), 0);
	if (dev->irq < 0)
		return dev->irq;

	ret = devm_request_threaded_irq(dev->base.dev, dev->irq,
					ethos_job_irq_handler,
					ethos_job_irq_handler_thread,
					IRQF_SHARED, KBUILD_MODNAME,
					dev);
	if (ret) {
		dev_err(dev->base.dev, "failed to request irq");
		return ret;
	}

	dev->reset.wq = alloc_ordered_workqueue("ethos-reset", 0);
	if (!dev->reset.wq)
		return -ENOMEM;

	dev->fence_context = dma_fence_context_alloc(1);

	args.timeout_wq = dev->reset.wq;
	ret = drm_sched_init(&dev->sched, &args);
	if (ret) {
		dev_err(dev->base.dev, "Failed to create scheduler: %d.", ret);
		goto err_sched;
	}

	return 0;

err_sched:
	drm_sched_fini(&dev->sched);

	destroy_workqueue(dev->reset.wq);
	return ret;
}

void ethos_job_fini(struct ethos_device *dev)
{
	drm_sched_fini(&dev->sched);

	cancel_work_sync(&dev->reset.work);
	destroy_workqueue(dev->reset.wq);
}

int ethos_job_open(struct ethos_file_priv *ethos_priv)
{
	struct ethos_device *dev = ethos_priv->edev;
	struct drm_gpu_scheduler *sched = &dev->sched;
	int ret;

	ret = drm_sched_entity_init(&ethos_priv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL,
				    &sched, 1, NULL);
	return WARN_ON(ret);
}

void ethos_job_close(struct ethos_file_priv *ethos_priv)
{
	struct drm_sched_entity *entity = &ethos_priv->sched_entity;

	drm_sched_entity_destroy(entity);
}

int ethos_job_is_idle(struct ethos_device *dev)
{
	/* If there are any jobs in this HW queue, we're not idle */
	if (atomic_read(&dev->sched.credit_count))
		return false;

	return true;
}

static int ethos_ioctl_submit_job(struct drm_device *dev, struct drm_file *file,
				   struct drm_ethos_job *job)
{
	struct ethos_device *edev = to_ethos_device(dev);
	struct ethos_file_priv *file_priv = file->driver_priv;
	struct ethos_job *ejob = NULL;
	struct ethos_validated_cmdstream_info *cmd_info;
	int ret = 0;

	/* BO region 2 is reserved if SRAM is used */
	if (job->region_bo_handles[ETHOS_SRAM_REGION] && job->sram_size)
		return -EINVAL;

	if (edev->npu_info.sram_size < job->sram_size)
		return -EINVAL;

	ejob = kzalloc(sizeof(*ejob), GFP_KERNEL);
	if (!ejob)
		return -ENOMEM;

	kref_init(&ejob->refcount);

	ejob->dev = edev;
	ejob->sram_size = job->sram_size;

	ret = drm_sched_job_init(&ejob->base,
				 &file_priv->sched_entity,
				 1, NULL);
	if (ret)
		goto out_put_job;

	ejob->cmd_bo = drm_gem_object_lookup(file, job->cmd_bo);
	cmd_info = to_ethos_bo(ejob->cmd_bo)->info;
	if (!ejob->cmd_bo)
		goto out_cleanup_job;

	for (int i = 0; i < NPU_BASEP_REGION_MAX; i++) {
		struct drm_gem_object *gem;

		if (job->region_bo_handles[i] == 0)
			continue;

		/* Don't allow a region to point to the cmd BO */
		if (job->region_bo_handles[i] == job->cmd_bo) {
			ret = -EINVAL;
			goto out_cleanup_job;
		}

		gem = drm_gem_object_lookup(file, job->region_bo_handles[i]);

		/* Verify the command stream doesn't have accesses outside the BO */
		if (cmd_info->region_size[i] > gem->size) {
			dev_err(dev->dev,
				"cmd stream region %d size greater than BO size (%llu > %zu)\n",
				i, cmd_info->region_size[i], gem->size);
			ret = -EOVERFLOW;
			goto out_cleanup_job;
		}

		ejob->region_bo[ejob->region_cnt] = gem;
		ejob->region_bo_num[ejob->region_cnt] = i;
		ejob->region_cnt++;
	}
	ret = ethos_job_push(ejob);
	if (ret)
		goto out_cleanup_job;

out_cleanup_job:
	if (ret)
		drm_sched_job_cleanup(&ejob->base);
out_put_job:
	ethos_job_put(ejob);

	return ret;
}

int ethos_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_ethos_submit *args = data;
	struct drm_ethos_job *jobs;
	int ret = 0;
	unsigned int i = 0;

	if (args->pad) {
		drm_dbg(dev, "Reserved field in drm_ethos_submit struct should be 0.\n");
		return -EINVAL;
	}

	jobs = kvmalloc_array(args->job_count, sizeof(*jobs), GFP_KERNEL);
	if (!jobs)
		return -ENOMEM;

	if (copy_from_user(jobs,
			   (void __user *)(uintptr_t)args->jobs,
			   args->job_count * sizeof(*jobs))) {
		ret = -EFAULT;
		drm_dbg(dev, "Failed to copy incoming job array\n");
		goto exit;
	}

	for (i = 0; i < args->job_count; i++) {
		ret = ethos_ioctl_submit_job(dev, file, &jobs[i]);
		if (ret)
			break;
	}

exit:
	kfree(jobs);

	return ret;
}
