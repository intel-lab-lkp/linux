// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"
#include "qda_memory_dma.h"
#include "qda_prime.h"

static void setup_vma_flags(struct vm_area_struct *vma)
{
	vm_flags_set(vma, VM_DONTEXPAND);
	vm_flags_set(vma, VM_DONTDUMP);
}

/**
 * qda_gem_free_object() - Free a GEM object and its associated resources
 * @gem_obj: DRM GEM object to free
 */
void qda_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct qda_gem_obj *qda_gem_obj = to_qda_gem_obj(gem_obj);
	struct qda_dev *qdev = qda_dev_from_drm(gem_obj->dev);

	if (qda_gem_obj->is_imported) {
		if (qda_gem_obj->attachment && qda_gem_obj->sgt)
			dma_buf_unmap_attachment_unlocked(qda_gem_obj->attachment,
							  qda_gem_obj->sgt, DMA_BIDIRECTIONAL);
		if (qda_gem_obj->attachment)
			dma_buf_detach(qda_gem_obj->dma_buf, qda_gem_obj->attachment);
		if (qda_gem_obj->dma_buf)
			dma_buf_put(qda_gem_obj->dma_buf);
		if (qda_gem_obj->iommu_dev && qdev->iommu_mgr)
			qda_memory_manager_free(qdev->iommu_mgr, qda_gem_obj);
	} else {
		if (qda_gem_obj->virt && qdev->iommu_mgr)
			qda_memory_manager_free(qdev->iommu_mgr, qda_gem_obj);
	}

	drm_gem_object_release(gem_obj);
	kfree(qda_gem_obj);
}

/**
 * qda_gem_mmap_obj() - Map a GEM object into userspace
 * @drm_obj: DRM GEM object to map
 * @vma: Virtual memory area to map into
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_gem_mmap_obj(struct drm_gem_object *drm_obj, struct vm_area_struct *vma)
{
	struct qda_gem_obj *qda_gem_obj = to_qda_gem_obj(drm_obj);
	int ret;

	/* Imported dma-buf objects must be mmap'd through the exporter, not the importer */
	if (qda_gem_obj->is_imported)
		return -EINVAL;

	/* Reset vm_pgoff for DMA mmap */
	vma->vm_pgoff = 0;

	ret = qda_dma_mmap(qda_gem_obj, vma);
	if (ret == 0)
		setup_vma_flags(vma);

	return ret;
}

static const struct drm_gem_object_funcs qda_gem_object_funcs = {
	.free = qda_gem_free_object,
	.mmap = qda_gem_mmap_obj,
};

/**
 * qda_gem_alloc_object() - Allocate a new QDA GEM object
 * @drm_dev: DRM device
 * @aligned_size: Size of the object in bytes (must be page-aligned)
 *
 * Return: Pointer to the new GEM object, or ERR_PTR on failure
 */
struct qda_gem_obj *qda_gem_alloc_object(struct drm_device *drm_dev, size_t aligned_size)
{
	struct qda_gem_obj *qda_gem_obj;
	int ret;

	qda_gem_obj = kzalloc_obj(*qda_gem_obj);
	if (!qda_gem_obj)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_object_init(drm_dev, &qda_gem_obj->base, aligned_size);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize GEM object: %d\n", ret);
		kfree(qda_gem_obj);
		return ERR_PTR(ret);
	}

	qda_gem_obj->base.funcs = &qda_gem_object_funcs;
	qda_gem_obj->size = aligned_size;

	drm_dbg_driver(drm_dev, "Allocated GEM object size=%zu\n", aligned_size);
	return qda_gem_obj;
}

void qda_gem_cleanup_object(struct qda_gem_obj *qda_gem_obj)
{
	drm_gem_object_release(&qda_gem_obj->base);
	kfree(qda_gem_obj);
}

struct drm_gem_object *qda_gem_lookup_object(struct drm_file *file_priv, u32 handle)
{
	struct drm_gem_object *gem_obj;

	gem_obj = drm_gem_object_lookup(file_priv, handle);
	if (!gem_obj)
		return ERR_PTR(-ENOENT);

	return gem_obj;
}

int qda_gem_create_handle(struct drm_file *file_priv, struct drm_gem_object *gem_obj, u32 *handle)
{
	int ret;

	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	drm_gem_object_put(gem_obj);

	return ret;
}

/**
 * qda_gem_create_object() - Allocate and initialize a GEM object with DMA backing
 * @drm_dev: DRM device
 * @iommu_mgr: Memory manager to use for DMA allocation
 * @size: Requested size in bytes
 * @file_priv: DRM file private data for process association
 *
 * Return: Pointer to the base DRM GEM object on success, ERR_PTR on failure
 */
struct drm_gem_object *qda_gem_create_object(struct drm_device *drm_dev,
					     struct qda_memory_manager *iommu_mgr, size_t size,
					     struct drm_file *file_priv)
{
	struct qda_gem_obj *qda_gem_obj;
	size_t aligned_size;
	int ret;

	if (size == 0) {
		drm_err(drm_dev, "Invalid size for GEM object creation\n");
		return ERR_PTR(-EINVAL);
	}

	aligned_size = PAGE_ALIGN(size);

	qda_gem_obj = qda_gem_alloc_object(drm_dev, aligned_size);
	if (IS_ERR(qda_gem_obj))
		return ERR_CAST(qda_gem_obj);
	qda_gem_obj->is_imported = false;
	qda_gem_obj->dma_buf = NULL;
	qda_gem_obj->attachment = NULL;
	qda_gem_obj->sgt = NULL;

	ret = qda_memory_manager_alloc(iommu_mgr, qda_gem_obj, file_priv);
	if (ret) {
		drm_err(drm_dev, "Memory manager allocation failed: %d\n", ret);
		qda_gem_cleanup_object(qda_gem_obj);
		return ERR_PTR(ret);
	}

	drm_dbg_driver(drm_dev, "GEM object created successfully size=%zu\n", aligned_size);
	return &qda_gem_obj->base;
}
