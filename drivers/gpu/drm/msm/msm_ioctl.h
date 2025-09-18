/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifndef __MSM_IOCTLS_H__
#define __MSM_IOCTLS_H__

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_gem_object;

int msm_ioctl_get_param(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_set_param(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_gem_new(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_gem_cpu_prep(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_gem_cpu_fini(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_gem_info_iova(struct drm_device *dev, struct drm_file *file,
			    struct drm_gem_object *obj, uint64_t *iova);
int msm_ioctl_gem_info_set_iova(struct drm_device *dev, struct drm_file *file,
				struct drm_gem_object *obj, uint64_t iova);
int msm_ioctl_gem_info_set_metadata(struct drm_gem_object *obj,
				    __user void *metadata, u32 metadata_size);
int msm_ioctl_gem_info_get_metadata(struct drm_gem_object *obj,
				    __user void *metadata, u32 *metadata_size);
int msm_ioctl_gem_info(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_wait_fence(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_gem_madvise(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_submitqueue_new(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_submitqueue_query(struct drm_device *dev, void *data, struct drm_file *file);
int msm_ioctl_submitqueue_close(struct drm_device *dev, void *data, struct drm_file *file);

#endif
