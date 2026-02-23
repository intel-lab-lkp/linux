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
