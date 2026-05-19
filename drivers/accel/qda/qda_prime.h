/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_PRIME_H__
#define __QDA_PRIME_H__

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <linux/dma-buf.h>

struct drm_gem_object *qda_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf);
int qda_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
			   int prime_fd, u32 *handle);

#endif /* __QDA_PRIME_H__ */
