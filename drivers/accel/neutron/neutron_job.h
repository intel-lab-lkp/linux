/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2025-2026 NXP */

#ifndef __NEUTRON_JOB_H__
#define __NEUTRON_JOB_H__

#include <linux/kref.h>
#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>
#include <drm/gpu_scheduler.h>
#include <drm/neutron_accel.h>

#include "neutron_driver.h"

struct neutron_device;
struct neutron_file_priv;

struct neutron_job {
	struct drm_sched_job base;
	struct neutron_device *ndev;
	struct dma_fence *neutron_fence;
	struct drm_gem_object *bo;
	enum drm_neutron_job_type type;
	union {
		struct drm_neutron_inference_job inference;
	};
	struct kref refcnt;
};

#define to_neutron_job(job) \
	container_of(job, struct neutron_job, base)

int neutron_job_init(struct neutron_device *dev);
void neutron_job_fini(struct neutron_device *dev);
int neutron_job_open(struct neutron_file_priv *npriv);
void neutron_job_close(struct neutron_file_priv *npriv);

void neutron_job_done_handler(struct neutron_device *dev);
void neutron_job_err_handler(struct neutron_device *dev);

int neutron_ioctl_submit_job(struct drm_device *dev, void *data, struct drm_file *filp);

#endif /* __NEUTRON_JOB_H__ */
