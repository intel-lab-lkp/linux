// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_ioctl.h>
#include <drm/qda_accel.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_ioctl.h"

/**
 * qda_ioctl_query() - Query DSP device information
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_query)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_query *args = data;
	struct qda_dev *qdev;

	qdev = qda_dev_from_drm(dev);

	strscpy(args->dsp_name, qdev->dsp_name, sizeof(args->dsp_name));

	return 0;
}

/**
 * qda_ioctl_gem_create() - Create a GEM buffer object
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_gem_create)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_create *args = data;
	struct drm_gem_object *gem_obj;
	struct qda_dev *qdev;

	if (args->pad)
		return -EINVAL;

	qdev = qda_dev_from_drm(dev);
	if (!qdev->iommu_mgr)
		return -ENODEV;

	gem_obj = qda_gem_create_object(dev, qdev->iommu_mgr, args->size, file_priv);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	return qda_gem_create_handle(file_priv, gem_obj, &args->handle);
}

/**
 * qda_ioctl_gem_mmap_offset() - Get the mmap offset for a GEM object
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_gem_mmap_offset)
 * @file_priv: DRM file private data
 *
 * Uses drm_gem_dumb_map_offset() which rejects imported dma-buf objects
 * (mmap of imported objects is not allowed).
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_mmap_offset *args = data;

	if (args->pad)
		return -EINVAL;

	return drm_gem_dumb_map_offset(file_priv, dev, args->handle, &args->offset);
}
