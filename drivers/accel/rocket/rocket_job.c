// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_print.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/rocket_accel.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "rocket_core.h"
#include "rocket_device.h"
#include "rocket_drv.h"
#include "rocket_job.h"
#include "rocket_registers.h"

#define JOB_TIMEOUT_MS 500

static struct rocket_task *
to_rocket_task(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct rocket_task, base);
}

static const char *rocket_fence_get_driver_name(struct dma_fence *fence)
{
	return "rocket";
}

static const char *rocket_fence_get_timeline_name(struct dma_fence *fence)
{
	return "rockchip-npu";
}

static const struct dma_fence_ops rocket_fence_ops = {
	.get_driver_name = rocket_fence_get_driver_name,
	.get_timeline_name = rocket_fence_get_timeline_name,
};

static struct dma_fence *rocket_fence_create(struct rocket_core *core)
{
	struct dma_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	dma_fence_init(fence, &rocket_fence_ops, &core->fence_lock,
		       core->fence_context, ++core->emit_seqno);

	return fence;
}

static int
rocket_copy_tasks(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_job *job,
		  struct rocket_job *rjob)
{
	if (job->task_struct_size < sizeof(struct drm_rocket_task))
		return -EINVAL;

	rjob->task_count = job->task_count;

	if (!rjob->task_count)
		return 0;

	rjob->tasks = kvzalloc_objs(*rjob->tasks, job->task_count);
	if (!rjob->tasks) {
		drm_dbg(dev, "Failed to allocate task array\n");
		return -ENOMEM;
	}

	for (int i = 0; i < rjob->task_count; i++) {
		struct drm_rocket_task task = {0};

		if (copy_from_user(&task,
				   u64_to_user_ptr(job->tasks) + i * job->task_struct_size,
				   sizeof(task))) {
			drm_dbg(dev, "Failed to copy incoming tasks\n");
			return -EFAULT;
		}

		if (task.regcmd_count == 0) {
			drm_dbg(dev, "regcmd_count field in drm_rocket_task should be > 0.\n");
			return -EINVAL;
		}

		rjob->tasks[i].regcmd = task.regcmd;
		rjob->tasks[i].regcmd_count = task.regcmd_count;
	}

	return 0;
}

static void rocket_job_hw_submit(struct rocket_core *core, struct rocket_task *task)
{
	unsigned int extra_bit;

	/* GO ! */

	rocket_pc_writel(core, BASE_ADDRESS, 0x1);

	 /* From rknpu, in the TRM this bit is marked as reserved */
	extra_bit = 0x10000000 * core->index;
	rocket_cna_writel(core, S_POINTER, CNA_S_POINTER_POINTER_PP_EN(1) |
					   CNA_S_POINTER_EXECUTER_PP_EN(1) |
					   CNA_S_POINTER_POINTER_PP_MODE(1) |
					   extra_bit);

	rocket_core_writel(core, S_POINTER, CORE_S_POINTER_POINTER_PP_EN(1) |
					    CORE_S_POINTER_EXECUTER_PP_EN(1) |
					    CORE_S_POINTER_POINTER_PP_MODE(1) |
					    extra_bit);

	rocket_pc_writel(core, BASE_ADDRESS, task->regcmd);
	rocket_pc_writel(core, REGISTER_AMOUNTS,
			 PC_REGISTER_AMOUNTS_PC_DATA_AMOUNT((task->regcmd_count + 1) / 2 - 1));

	rocket_pc_writel(core, INTERRUPT_MASK, PC_INTERRUPT_MASK_DPU_0 | PC_INTERRUPT_MASK_DPU_1);
	rocket_pc_writel(core, INTERRUPT_CLEAR, PC_INTERRUPT_CLEAR_DPU_0 | PC_INTERRUPT_CLEAR_DPU_1);

	rocket_pc_writel(core, TASK_CON, PC_TASK_CON_RESERVED_0(1) |
					 PC_TASK_CON_TASK_COUNT_CLEAR(1) |
					 PC_TASK_CON_TASK_NUMBER(1) |
					 PC_TASK_CON_TASK_PP_EN(1));

	rocket_pc_writel(core, TASK_DMA_BASE_ADDR, PC_TASK_DMA_BASE_ADDR_DMA_BASE_ADDR(0x0));

	rocket_pc_writel(core, OPERATION_ENABLE, PC_OPERATION_ENABLE_OP_EN(1));

	dev_dbg(core->dev, "Submitted regcmd at 0x%llx to core %d", task->regcmd, core->index);
}

static int rocket_acquire_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct drm_sched_job *job,
					bool is_write)
{
	int i, ret;

	for (i = 0; i < bo_count; i++) {
		ret = dma_resv_reserve_fences(bos[i]->resv, 1);
		if (ret)
			return ret;

		ret = drm_sched_job_add_implicit_dependencies(job, bos[i],
							      is_write);
		if (ret)
			return ret;
	}

	return 0;
}

static void rocket_attach_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct dma_fence *fence)
{
	int i;

	for (i = 0; i < bo_count; i++)
		dma_resv_add_fence(bos[i]->resv, fence, DMA_RESV_USAGE_WRITE);
}

static int rocket_job_push(struct rocket_job *job)
{
	struct rocket_device *rdev = job->rdev;
	unsigned int bo_count = job->in_bo_count + job->out_bo_count;
	struct rocket_task *last = &job->tasks[job->task_count - 1];
	struct drm_gem_object **bos;
	struct ww_acquire_ctx acquire_ctx;
	int ret, i;

	bos = kvmalloc_array(bo_count, sizeof(void *), GFP_KERNEL);
	if (!bos) {
		ret = -ENOMEM;
		goto err_cleanup_tasks;
	}
	memcpy(bos, job->in_bos, job->in_bo_count * sizeof(void *));
	memcpy(&bos[job->in_bo_count], job->out_bos, job->out_bo_count * sizeof(void *));

	ret = drm_gem_lock_reservations(bos, bo_count, &acquire_ctx);
	if (ret)
		goto err_free_bos;

	/* Anchor the BO synchronization on the last task: its finished fence is
	 * the inference's completion fence.
	 */
	ret = rocket_acquire_object_fences(job->in_bos, job->in_bo_count,
					   &last->base, false);
	if (ret)
		goto err_unlock;

	ret = rocket_acquire_object_fences(job->out_bos, job->out_bo_count,
					   &last->base, true);
	if (ret)
		goto err_unlock;

	mutex_lock(&rdev->sched_lock);

	for (i = 0; i < job->task_count; i++) {
		drm_sched_job_arm(&job->tasks[i].base);

		if (i == job->task_count - 1)
			job->inference_done_fence = dma_fence_get(&last->base.s_fence->finished);

		/* put by scheduler job completion */
		kref_get(&job->refcount);

		drm_sched_entity_push_job(&job->tasks[i].base);
	}

	mutex_unlock(&rdev->sched_lock);

	rocket_attach_object_fences(job->out_bos, job->out_bo_count,
				    job->inference_done_fence);

	drm_gem_unlock_reservations(bos, bo_count, &acquire_ctx);
	kvfree(bos);

	return 0;

err_unlock:
	drm_gem_unlock_reservations(bos, bo_count, &acquire_ctx);
err_free_bos:
	kvfree(bos);
err_cleanup_tasks:
	for (i = 0; i < job->task_count; i++)
		drm_sched_job_cleanup(&job->tasks[i].base);

	return ret;
}

static void rocket_job_cleanup(struct kref *ref)
{
	struct rocket_job *job = container_of(ref, struct rocket_job,
						refcount);
	unsigned int i;

	/*
	 * The last task holding a reference is gone, so the inference is over.
	 * The ordered scheduler workqueue runs this (from free_job()) before
	 * the next inference's run_job(), so the core's IOMMU group is free
	 * in time for it.
	 */
	if (job->core)
		iommu_detach_group(NULL, job->core->iommu_group);

	rocket_iommu_domain_put(job->domain);

	if (job->tasks) {
		for (i = 0; i < job->task_count; i++)
			dma_fence_put(job->tasks[i].done_fence);
		dma_fence_put(job->inference_done_fence);
	}

	if (job->in_bos) {
		for (i = 0; i < job->in_bo_count; i++)
			drm_gem_object_put(job->in_bos[i]);

		kvfree(job->in_bos);
	}

	if (job->out_bos) {
		for (i = 0; i < job->out_bo_count; i++)
			drm_gem_object_put(job->out_bos[i]);

		kvfree(job->out_bos);
	}

	kvfree(job->tasks);

	kfree(job);
}

static void rocket_job_put(struct rocket_job *job)
{
	kref_put(&job->refcount, rocket_job_cleanup);
}

static void rocket_job_free(struct drm_sched_job *sched_job)
{
	struct rocket_task *task = to_rocket_task(sched_job);

	drm_sched_job_cleanup(sched_job);

	rocket_job_put(task->job);
}

static struct rocket_core *sched_to_core(struct rocket_device *rdev,
					 struct drm_gpu_scheduler *sched)
{
	unsigned int core;

	for (core = 0; core < rdev->num_cores; core++) {
		if (&rdev->cores[core].sched == sched)
			return &rdev->cores[core];
	}

	return NULL;
}

static struct dma_fence *rocket_job_run(struct drm_sched_job *sched_job)
{
	struct rocket_task *task = to_rocket_task(sched_job);
	struct rocket_job *job = task->job;
	struct rocket_core *core = sched_to_core(job->rdev, sched_job->sched);
	struct dma_fence *fence = NULL;
	int ret;

	if (unlikely(sched_job->s_fence->finished.error))
		return NULL;

	/* An earlier task of this inference timed out, abandon the inference. */
	if (atomic_read(&job->cancelled)) {
		dma_fence_set_error(&sched_job->s_fence->finished, -ECANCELED);
		return NULL;
	}

	fence = rocket_fence_create(core);
	if (IS_ERR(fence))
		return fence;

	if (task->done_fence)
		dma_fence_put(task->done_fence);
	task->done_fence = dma_fence_get(fence);

	ret = pm_runtime_resume_and_get(core->dev);
	if (ret < 0) {
		dma_fence_put(fence);
		return ERR_PTR(ret);
	}

	/* Attach the domain once for the whole inference. */
	if (!job->core) {
		ret = iommu_attach_group(job->domain->domain, core->iommu_group);
		if (ret < 0) {
			pm_runtime_put_autosuspend(core->dev);
			dma_fence_put(fence);
			return ERR_PTR(ret);
		}
		job->core = core;
	}

	WRITE_ONCE(core->in_flight_task, task);
	rocket_job_hw_submit(core, task);

	return fence;
}

static void rocket_job_handle_irq(struct rocket_core *core)
{
	struct rocket_task *task;

	pm_runtime_mark_last_busy(core->dev);

	rocket_pc_writel(core, OPERATION_ENABLE, 0x0);
	rocket_pc_writel(core, INTERRUPT_CLEAR, 0x1ffff);

	/*
	 * Claim the in-flight task: the reset path may run concurrently, so
	 * whichever of us wins owns the PM put.
	 */
	task = xchg(&core->in_flight_task, NULL);
	if (task) {
		pm_runtime_put_autosuspend(core->dev);
		dma_fence_signal(task->done_fence);
	}
}

static void
rocket_reset(struct rocket_core *core, struct drm_sched_job *bad)
{
	struct rocket_task *task;

	drm_sched_stop(&core->sched, bad);

	/* Claim the in-flight task (see rocket_job_handle_irq()). */
	task = xchg(&core->in_flight_task, NULL);
	if (task)
		pm_runtime_put_noidle(core->dev);

	/* Proceed with reset now. */
	rocket_core_reset(core);

	/* Restart the scheduler */
	drm_sched_start(&core->sched, 0);
}

static enum drm_gpu_sched_stat rocket_job_timedout(struct drm_sched_job *sched_job)
{
	struct rocket_task *task = to_rocket_task(sched_job);
	struct rocket_job *job = task->job;
	struct rocket_core *core = sched_to_core(job->rdev, sched_job->sched);

	dev_err(core->dev, "NPU job timed out");

	/* Abandon the rest of the inference before the scheduler is restarted. */
	atomic_set(&job->cancelled, 1);

	rocket_reset(core, sched_job);

	return DRM_GPU_SCHED_STAT_RESET;
}

static const struct drm_sched_backend_ops rocket_sched_ops = {
	.run_job = rocket_job_run,
	.timedout_job = rocket_job_timedout,
	.free_job = rocket_job_free
};

static irqreturn_t rocket_job_irq_handler_thread(int irq, void *data)
{
	struct rocket_core *core = data;

	rocket_job_handle_irq(core);

	return IRQ_HANDLED;
}

static irqreturn_t rocket_job_irq_handler(int irq, void *data)
{
	struct rocket_core *core = data;
	u32 raw_status = rocket_pc_readl(core, INTERRUPT_RAW_STATUS);

	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_READ_ERROR);
	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_WRITE_ERROR);

	if (!(raw_status & PC_INTERRUPT_RAW_STATUS_DPU_0 ||
	      raw_status & PC_INTERRUPT_RAW_STATUS_DPU_1))
		return IRQ_NONE;

	rocket_pc_writel(core, INTERRUPT_MASK, 0x0);

	return IRQ_WAKE_THREAD;
}

int rocket_job_init(struct rocket_core *core)
{
	struct drm_sched_init_args args = {
		.ops = &rocket_sched_ops,
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(JOB_TIMEOUT_MS),
		.name = dev_name(core->dev),
		.dev = core->dev,
	};
	int ret;

	spin_lock_init(&core->fence_lock);

	core->irq = platform_get_irq(to_platform_device(core->dev), 0);
	if (core->irq < 0)
		return core->irq;

	ret = devm_request_threaded_irq(core->dev, core->irq,
					rocket_job_irq_handler,
					rocket_job_irq_handler_thread,
					IRQF_SHARED, dev_name(core->dev),
					core);
	if (ret) {
		dev_err(core->dev, "failed to request job irq");
		return ret;
	}

	core->reset.wq = alloc_ordered_workqueue("rocket-reset-%d", 0, core->index);
	if (!core->reset.wq)
		return -ENOMEM;

	core->fence_context = dma_fence_context_alloc(1);

	args.timeout_wq = core->reset.wq;
	ret = drm_sched_init(&core->sched, &args);
	if (ret) {
		dev_err(core->dev, "Failed to create scheduler: %d.", ret);
		goto err_sched;
	}

	return 0;

err_sched:
	drm_sched_fini(&core->sched);

	destroy_workqueue(core->reset.wq);
	return ret;
}

void rocket_job_fini(struct rocket_core *core)
{
	drm_sched_fini(&core->sched);

	destroy_workqueue(core->reset.wq);
}

int rocket_job_open(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_gpu_scheduler **scheds = kmalloc_objs(*scheds,
							 rdev->num_cores);
	unsigned int core;
	int ret;

	for (core = 0; core < rdev->num_cores; core++)
		scheds[core] = &rdev->cores[core].sched;

	ret = drm_sched_entity_init(&rocket_priv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL,
				    scheds,
				    rdev->num_cores, NULL);
	if (WARN_ON(ret))
		return ret;

	return 0;
}

void rocket_job_close(struct rocket_file_priv *rocket_priv)
{
	struct drm_sched_entity *entity = &rocket_priv->sched_entity;

	kfree(entity->sched_list);
	drm_sched_entity_destroy(entity);
}

int rocket_job_is_idle(struct rocket_core *core)
{
	/* If there are any jobs in this HW queue, we're not idle */
	if (atomic_read(&core->sched.credit_count))
		return false;

	return true;
}

static int rocket_ioctl_submit_job(struct drm_device *dev, struct drm_file *file,
				   struct drm_rocket_job *job)
{
	struct rocket_device *rdev = to_rocket_device(dev);
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct rocket_job *rjob = NULL;
	int i, ret = 0;

	if (job->task_count == 0)
		return -EINVAL;

	rjob = kzalloc_obj(*rjob);
	if (!rjob)
		return -ENOMEM;

	kref_init(&rjob->refcount);
	atomic_set(&rjob->cancelled, 0);
	rjob->rdev = rdev;

	ret = rocket_copy_tasks(dev, file, job, rjob);
	if (ret)
		goto out_put_job;

	for (i = 0; i < rjob->task_count; i++) {
		ret = drm_sched_job_init(&rjob->tasks[i].base, &file_priv->sched_entity,
					 1, NULL, file->client_id);
		if (ret)
			goto out_cleanup_tasks;

		rjob->tasks[i].job = rjob;
	}

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->in_bo_handles),
				     job->in_bo_handle_count, &rjob->in_bos);
	if (ret)
		goto out_cleanup_tasks;

	rjob->in_bo_count = job->in_bo_handle_count;

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->out_bo_handles),
				     job->out_bo_handle_count, &rjob->out_bos);
	if (ret)
		goto out_cleanup_tasks;

	rjob->out_bo_count = job->out_bo_handle_count;

	rjob->domain = rocket_iommu_domain_get(file_priv);

	ret = rocket_job_push(rjob);

	goto out_put_job;

out_cleanup_tasks:
	for (i--; i >= 0; i--)
		drm_sched_job_cleanup(&rjob->tasks[i].base);
out_put_job:
	rocket_job_put(rjob);

	return ret;
}

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_submit *args = data;
	struct drm_rocket_job *jobs;
	int ret = 0;
	unsigned int i = 0;

	if (args->job_count == 0)
		return 0;

	if (args->job_struct_size < sizeof(struct drm_rocket_job)) {
		drm_dbg(dev, "job_struct_size field in drm_rocket_submit struct is too small.\n");
		return -EINVAL;
	}

	if (args->reserved != 0) {
		drm_dbg(dev, "Reserved field in drm_rocket_submit struct should be 0.\n");
		return -EINVAL;
	}

	jobs = kvmalloc_objs(*jobs, args->job_count);
	if (!jobs) {
		drm_dbg(dev, "Failed to allocate incoming job array\n");
		return -ENOMEM;
	}

	for (i = 0; i < args->job_count; i++) {
		if (copy_from_user(&jobs[i],
				   u64_to_user_ptr(args->jobs) + i * args->job_struct_size,
				   sizeof(*jobs))) {
			ret = -EFAULT;
			drm_dbg(dev, "Failed to copy incoming job array\n");
			goto exit;
		}
	}


	for (i = 0; i < args->job_count; i++)
		rocket_ioctl_submit_job(dev, file, &jobs[i]);

exit:
	kvfree(jobs);

	return ret;
}
