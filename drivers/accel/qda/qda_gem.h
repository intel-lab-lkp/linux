/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __QDA_GEM_H__
#define __QDA_GEM_H__

#include <linux/dma-mapping.h>
#include <linux/xarray.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include "qda_memory_manager.h"

/**
 * struct qda_gem_obj - QDA GEM buffer object
 *
 * Represents a GEM buffer object that can be allocated by the driver
 * or imported from another driver via DMA-BUF.
 */
struct qda_gem_obj {
	/** @base: DRM GEM object base — must be first member */
	struct drm_gem_object base;
	/** @iommu_dev: IOMMU context bank device that performed the allocation */
	struct qda_iommu_device *iommu_dev;
	/** @dma_buf: Reference to imported dma_buf */
	struct dma_buf *dma_buf;
	/** @attachment: DMA buf attachment */
	struct dma_buf_attachment *attachment;
	/** @sgt: Scatter-gather table */
	struct sg_table *sgt;
	/** @virt: Kernel virtual address of the allocated DMA memory */
	void *virt;
	/** @dma_addr: DMA address (with SID encoded in upper 32 bits) */
	dma_addr_t dma_addr;
	/** @size: Size of the buffer in bytes */
	size_t size;
	/** @is_imported: True if buffer is imported, false if allocated */
	bool is_imported;
};

/**
 * to_qda_gem_obj - Cast a drm_gem_object pointer to qda_gem_obj
 * @gem_obj: Pointer to the embedded drm_gem_object
 */
#define to_qda_gem_obj(gem_obj) container_of(gem_obj, struct qda_gem_obj, base)

/* GEM object lifecycle */
struct drm_gem_object *qda_gem_create_object(struct drm_device *drm_dev,
					     struct qda_memory_manager *iommu_mgr,
					     size_t size, struct drm_file *file_priv);
void qda_gem_free_object(struct drm_gem_object *gem_obj);
int qda_gem_mmap_obj(struct drm_gem_object *gem_obj, struct vm_area_struct *vma);

/* Internal helpers (also used by PRIME import) */
struct qda_gem_obj *qda_gem_alloc_object(struct drm_device *drm_dev, size_t aligned_size);
void qda_gem_cleanup_object(struct qda_gem_obj *qda_gem_obj);

/* Utility functions */
struct drm_gem_object *qda_gem_lookup_object(struct drm_file *file_priv, u32 handle);
int qda_gem_create_handle(struct drm_file *file_priv, struct drm_gem_object *gem_obj, u32 *handle);

#endif /* __QDA_GEM_H__ */
