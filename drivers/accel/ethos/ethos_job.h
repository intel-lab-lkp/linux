/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */
/* Copyright 2025 Arm, Ltd. */

#ifndef __ETHOS_JOB_H__
#define __ETHOS_JOB_H__

#include <linux/kref.h>
#include <drm/gpu_scheduler.h>

struct ethos_device;
struct ethos_file_priv;

struct ethos_job {
	struct drm_sched_job base;
	struct ethos_device *dev;

	struct drm_gem_object *cmd_bo;
	struct drm_gem_object *region_bo[NPU_BASEP_REGION_MAX];
	u8 region_bo_num[NPU_BASEP_REGION_MAX];
	u8 region_cnt;
	u32 sram_size;

	/* Fence to be signaled by drm-sched once its done with the job */
	struct dma_fence *inference_done_fence;

	/* Fence to be signaled by IRQ handler when the job is complete. */
	struct dma_fence *done_fence;

	struct kref refcount;
};

int ethos_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);

int ethos_job_init(struct ethos_device *dev);
void ethos_job_fini(struct ethos_device *dev);
int ethos_job_open(struct ethos_file_priv *ethos_priv);
void ethos_job_close(struct ethos_file_priv *ethos_priv);
int ethos_job_is_idle(struct ethos_device *dev);

#endif
