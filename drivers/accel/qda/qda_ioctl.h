/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_IOCTL_H__
#define __QDA_IOCTL_H__

#include "qda_drv.h"

int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv);
int qda_ioctl_init_create(struct drm_device *dev, void *data, struct drm_file *file_priv);
int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv);
int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv);
int qda_ioctl_invoke(struct drm_device *dev, void *data, struct drm_file *file_priv);
int qda_ioctl_mmap(struct drm_device *dev, void *data, struct drm_file *file_priv);

#endif /* __QDA_IOCTL_H__ */
