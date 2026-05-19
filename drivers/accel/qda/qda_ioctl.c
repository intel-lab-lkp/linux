// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_ioctl.h>
#include <drm/qda_accel.h>
#include "qda_drv.h"
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
