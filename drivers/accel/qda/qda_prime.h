/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _QDA_PRIME_H
#define _QDA_PRIME_H

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <linux/dma-buf.h>

/**
 * qda_gem_prime_import - Import a DMA-BUF as a GEM object
 * @dev: DRM device structure
 * @dma_buf: DMA-BUF to import
 *
 * This function imports an external DMA-BUF into the QDA driver as a GEM
 * object. It handles both re-imports of buffers originally from this driver
 * and imports of external buffers from other drivers.
 *
 * Return: Pointer to the imported GEM object on success, ERR_PTR on failure
 */
struct drm_gem_object *qda_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf);

/**
 * qda_prime_fd_to_handle - Core implementation for PRIME FD to GEM handle conversion
 * @dev: DRM device structure
 * @file_priv: DRM file private data
 * @prime_fd: File descriptor of the PRIME buffer
 * @handle: Output parameter for the GEM handle
 *
 * This core function sets up the necessary context before calling the
 * DRM framework's prime FD to handle conversion. It ensures proper IOMMU
 * device assignment and tracking for the import operation.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
			   int prime_fd, u32 *handle);

#endif /* _QDA_PRIME_H */
