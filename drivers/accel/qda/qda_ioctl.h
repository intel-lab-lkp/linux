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

/**
 * qda_ioctl_prime_fd_to_handle - IOCTL handler for PRIME FD to handle conversion
 * @dev: DRM device structure
 * @file_priv: DRM file private data
 * @prime_fd: File descriptor of the PRIME buffer
 * @handle: Output parameter for the GEM handle
 *
 * This IOCTL handler converts a PRIME file descriptor to a GEM handle.
 * It serves as both the DRM driver callback and can be used directly.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
				 int prime_fd, u32 *handle);

/**
 * qda_ioctl_attach - Attach to DSP root protection domain
 * @dev: DRM device structure
 * @data: User-space data for the attach operation
 * @file_priv: DRM file private data
 *
 * This IOCTL handler attaches to the DSP root PD (Protection Domain)
 * to enable communication between the host and DSP.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_attach(struct drm_device *dev, void *data, struct drm_file *file_priv);

/**
 * fastrpc_release_current_dsp_process - Release DSP process resources
 * @qdev: QDA device structure
 * @file_priv: DRM file private data
 *
 * This function releases all resources associated with a DSP process
 * when a user-space client closes its file descriptor.
 *
 * Return: 0 on success, negative error code on failure
 */
int fastrpc_release_current_dsp_process(struct qda_dev *qdev, struct drm_file *file_priv);

/**
 * qda_ioctl_invoke - Invoke a remote procedure on the DSP
 * @dev: DRM device structure
 * @data: User-space data containing invocation parameters
 * @file_priv: DRM file private data
 *
 * This IOCTL handler initiates a remote procedure call on the DSP,
 * marshalling arguments, executing the call, and returning results.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_invoke(struct drm_device *dev, void *data, struct drm_file *file_priv);

#endif /* _QDA_IOCTL_H */
