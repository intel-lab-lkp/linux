/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_JOB_H__
#define __ROCKET_JOB_H__

#include <drm/drm_drv.h>
#include <drm/gpu_scheduler.h>

#include "rocket_core.h"
#include "rocket_drv.h"

/*
 * A task is one hardware submission. Each task is its own drm_sched_job,
 * so every submission to the NPU flows through run_job().
 */
struct rocket_task {
	struct drm_sched_job base;

	/* Inference this task belongs to. */
	struct rocket_job *job;

	u64 regcmd;
	u32 regcmd_count;

	/* Fence signaled by the IRQ handler once the hardware completes it. */
	struct dma_fence *done_fence;
};

/* An inference: the whole userspace submission, made of one or more tasks. */
struct rocket_job {
	struct kref refcount;

	/* Set when the inference must be abandoned (a task timed out). */
	atomic_t cancelled;

	struct rocket_device *rdev;

	struct drm_gem_object **in_bos;
	struct drm_gem_object **out_bos;

	u32 in_bo_count;
	u32 out_bo_count;

	struct rocket_task *tasks;
	u32 task_count;

	struct dma_fence *inference_done_fence;

	struct rocket_iommu_domain *domain;

	/* Core the domain is attached to. */
	struct rocket_core *core;
};

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);

int rocket_job_init(struct rocket_core *core);
void rocket_job_fini(struct rocket_core *core);
int rocket_job_open(struct rocket_file_priv *rocket_priv);
void rocket_job_close(struct rocket_file_priv *rocket_priv);
int rocket_job_is_idle(struct rocket_core *core);

#endif
