/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 Arm, Ltd. */

#ifndef __ETHOS_GEM_H__
#define __ETHOS_GEM_H__

#include "ethos_device.h"
#include <drm/drm_gem_dma_helper.h>

struct ethos_validated_cmdstream_info {
	u32 cmd_size;
	u64 region_size[NPU_BASEP_REGION_MAX];
	bool output_region[NPU_BASEP_REGION_MAX];
};

/**
 * struct ethos_gem_object - Driver specific GEM object.
 */
struct ethos_gem_object {
	/** @base: Inherit from drm_gem_shmem_object. */
	struct drm_gem_dma_object base;

	struct ethos_validated_cmdstream_info *info;

	/** @flags: Combination of drm_ethos_bo_flags flags. */
	u32 flags;
};

static inline
struct ethos_gem_object *to_ethos_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_dma_obj(obj), struct ethos_gem_object, base);
}

struct drm_gem_object *ethos_gem_create_object(struct drm_device *ddev,
					       size_t size);

int ethos_gem_create_with_handle(struct drm_file *file,
				 struct drm_device *ddev,
				 u64 *size, u32 flags, uint32_t *handle);

int ethos_gem_cmdstream_create(struct drm_file *file,
			       struct drm_device *ddev,
			       u32 size, u64 data, u32 flags, u32 *handle);

#endif /* __ETHOS_GEM_H__ */
