/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef _QDA_GEM_H
#define _QDA_GEM_H

#include <linux/xarray.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <linux/dma-mapping.h>

/* Forward declarations */
struct qda_memory_manager;
struct qda_iommu_device;

/**
 * struct qda_gem_obj - QDA GEM buffer object
 *
 * This structure represents a GEM buffer object that can be either
 * allocated by the driver or imported from another driver via dma-buf.
 */
struct qda_gem_obj {
	/* DRM GEM object base structure */
	struct drm_gem_object base;
	/* Kernel virtual address of allocated memory */
	void *virt;
	/* DMA address for allocated buffers */
	dma_addr_t dma_addr;
	/* Size of the buffer in bytes */
	size_t size;
	/* IOMMU device that performed the allocation */
	struct qda_iommu_device *iommu_dev;
	/* True if buffer is imported, false if allocated */
	bool is_imported;
	/* Reference to imported dma_buf */
	struct dma_buf *dma_buf;
	/* DMA buf attachment */
	struct dma_buf_attachment *attachment;
	/* Scatter-gather table */
	struct sg_table *sgt;
	/* DMA address of imported buffer */
	dma_addr_t imported_dma_addr;
};

/*
 * Helper macro to cast a drm_gem_object to qda_gem_obj
 */
#define to_qda_gem_obj(gem_obj) container_of(gem_obj, struct qda_gem_obj, base)

/*
 * GEM object lifecycle management
 */
struct drm_gem_object *qda_gem_create_object(struct drm_device *drm_dev,
					     struct qda_memory_manager *iommu_mgr,
					     size_t size, struct drm_file *file_priv);
void qda_gem_free_object(struct drm_gem_object *gem_obj);
int qda_gem_mmap_obj(struct drm_gem_object *gem_obj, struct vm_area_struct *vma);

/*
 * GEM IOCTL handlers
 */

/**
 * qda_ioctl_gem_create - Create a GEM buffer object
 * @dev: DRM device structure
 * @data: User-space data containing buffer creation parameters
 * @file_priv: DRM file private data
 *
 * This IOCTL handler creates a new GEM buffer object with the specified
 * size and returns a handle to the created buffer.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv);

/**
 * qda_ioctl_gem_mmap_offset - Get mmap offset for a GEM buffer object
 * @dev: DRM device structure
 * @data: User-space data containing buffer handle and offset result
 * @file_priv: DRM file private data
 *
 * This IOCTL handler retrieves the mmap offset for a GEM buffer object,
 * which can be used to map the buffer into user-space memory.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv);

/*
 * Helper functions for GEM object allocation and cleanup
 * These are used internally and by the PRIME import code
 */
struct qda_gem_obj *qda_gem_alloc_object(struct drm_device *drm_dev, size_t aligned_size);
void qda_gem_cleanup_object(struct qda_gem_obj *qda_gem_obj);

/*
 * Utility functions for GEM operations
 */
struct drm_gem_object *qda_gem_lookup_object(struct drm_file *file_priv, u32 handle);
int qda_gem_create_handle(struct drm_file *file_priv, struct drm_gem_object *gem_obj, u32 *handle);

#endif /* _QDA_GEM_H */
