// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2025-2026 NXP */

#include <linux/sizes.h>
#include <linux/align.h>
#include <linux/dma-map-ops.h>
#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/neutron_accel.h>

#include "neutron_device.h"
#include "neutron_gem.h"

#define NEUTRON_BO_ALIGN SZ_1M

struct drm_gem_object *neutron_gem_create_object(struct drm_device *drm, size_t size)
{
	struct neutron_device *ndev = to_neutron_device(drm);
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;

	dma_obj = kzalloc_obj(*dma_obj);
	if (!dma_obj)
		return ERR_PTR(-ENOMEM);

	dma_obj->map_noncoherent = !dev_is_dma_coherent(ndev->dev);
	dma_obj->map_bidirectional = true;
	gem_obj = &dma_obj->base;

	return gem_obj;
}

int neutron_ioctl_create_bo(struct drm_device *drm, void *data, struct drm_file *filp)
{
	struct drm_neutron_create_bo *args = data;
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	size_t size;
	int ret;

	if (!args->size || args->pad)
		return -EINVAL;

	size = ALIGN(args->size, NEUTRON_BO_ALIGN);

	dma_obj = drm_gem_dma_create(drm, size);
	if (IS_ERR(dma_obj))
		return PTR_ERR(dma_obj);

	gem_obj = &dma_obj->base;

	/* We expect correctly aligned buffers, but double-check */
	if (drm_WARN_ON(drm, !IS_ALIGNED(dma_obj->dma_addr, NEUTRON_BO_ALIGN))) {
		ret = -EFAULT;
		goto out_put;
	}

	ret = drm_gem_handle_create(filp, gem_obj, &args->handle);
	if (ret)
		goto out_put;

	args->map_offset = drm_vma_node_offset_addr(&gem_obj->vma_node);
	args->size = gem_obj->size;

out_put:
	/* No need to keep a reference of the GEM object. Freeing is handled by user */
	drm_gem_object_put(gem_obj);

	return ret;
}

int neutron_ioctl_sync_bo(struct drm_device *drm, void *data, struct drm_file *filp)
{
	struct drm_neutron_sync_bo *args = data;
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	dma_addr_t start_addr;
	int ret = 0;

	gem_obj = drm_gem_object_lookup(filp, args->handle);
	if (!gem_obj) {
		dev_dbg(drm->dev, "Invalid BO handle %u\n", args->handle);
		return -ENOENT;
	}

	dma_obj = to_drm_gem_dma_obj(gem_obj);

	if (!args->size || args->offset >= gem_obj->size ||
	    args->size > gem_obj->size - args->offset) {
		dev_dbg(drm->dev, "Invalid offset/size for BO sync\n");
		ret = -EINVAL;
		goto out_put;
	}

	start_addr = dma_obj->dma_addr + args->offset;

	switch (args->direction) {
	case DRM_NEUTRON_SYNC_TO_DEVICE:
		dma_sync_single_for_device(drm->dev, start_addr, args->size,
					   DMA_BIDIRECTIONAL);
		break;
	case DRM_NEUTRON_SYNC_FROM_DEVICE:
		dma_sync_single_for_cpu(drm->dev, start_addr, args->size,
					DMA_BIDIRECTIONAL);
		break;
	default:
		dev_dbg(drm->dev, "Invalid direction for BO sync\n");
		ret = -EINVAL;
	}

out_put:
	drm_gem_object_put(gem_obj);

	return ret;
}
