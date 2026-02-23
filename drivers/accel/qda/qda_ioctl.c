// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>
#include <drm/qda_accel.h>
#include "qda_drv.h"
#include "qda_ioctl.h"

static int qda_validate_and_get_context(struct drm_device *dev, struct drm_file *file_priv,
					struct qda_dev **qdev, struct qda_user **qda_user)
{
	struct qda_drm_priv *drm_priv = dev->dev_private;
	struct qda_file_priv *qda_file_priv;

	if (!drm_priv)
		return -EINVAL;

	*qdev = drm_priv->qdev;
	if (!*qdev)
		return -EINVAL;

	qda_file_priv = (struct qda_file_priv *)file_priv->driver_priv;
	if (!qda_file_priv || !qda_file_priv->qda_user)
		return -EINVAL;

	*qda_user = qda_file_priv->qda_user;

	return 0;
}

int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qda_dev *qdev;
	struct qda_user *qda_user;
	struct drm_qda_query *args = data;
	int ret;

	ret = qda_validate_and_get_context(dev, file_priv, &qdev, &qda_user);
	if (ret)
		return ret;

	strscpy(args->dsp_name, qdev->dsp_name, sizeof(args->dsp_name));

	return 0;
}

int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_create *args = data;
	struct drm_gem_object *gem_obj;
	struct qda_drm_priv *drm_priv;

	drm_priv = get_drm_priv_from_device(dev);
	if (!drm_priv || !drm_priv->iommu_mgr)
		return -EINVAL;

	gem_obj = qda_gem_create_object(dev, drm_priv->iommu_mgr, args->size, file_priv);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	return qda_gem_create_handle(file_priv, gem_obj, &args->handle);
}

int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_mmap_offset *args = data;
	struct drm_gem_object *gem_obj;
	int ret;

	gem_obj = qda_gem_lookup_object(file_priv, args->handle);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret == 0)
		args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	drm_gem_object_put(gem_obj);
	return ret;
}
