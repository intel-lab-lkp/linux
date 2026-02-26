/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2025-2026 NXP */

#ifndef __NEUTRON_GEM_H__
#define __NEUTRON_GEM_H__

#include <drm/drm_gem.h>

struct drm_gem_object *neutron_gem_create_object(struct drm_device *drm, size_t size);

int neutron_ioctl_create_bo(struct drm_device *drm, void *data, struct drm_file *filp);
int neutron_ioctl_sync_bo(struct drm_device *drm, void *data, struct drm_file *filp);

#endif /* __NEUTRON_GEM_H__ */
