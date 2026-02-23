/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _QDA_IOCTL_H
#define _QDA_IOCTL_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <drm/drm_ioctl.h>
#include "qda_drv.h"

/**
 * qda_ioctl_query - Query DSP device information and capabilities
 * @dev: DRM device structure
 * @data: User-space data containing query parameters and results
 * @file_priv: DRM file private data
 *
 * This IOCTL handler queries information about the DSP device.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv);

#endif /* _QDA_IOCTL_H */
