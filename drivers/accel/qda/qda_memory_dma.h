/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _QDA_MEMORY_DMA_H
#define _QDA_MEMORY_DMA_H

#include <linux/dma-mapping.h>
#include "qda_memory_manager.h"

/**
 * qda_dma_alloc() - Allocate DMA coherent memory for a GEM object
 * @iommu_dev: Pointer to the QDA IOMMU device structure
 * @gem_obj: Pointer to GEM object to allocate memory for
 * @size: Size of memory to allocate in bytes
 *
 * Allocates DMA-coherent memory and sets up the GEM object with the
 * allocated memory details including virtual and DMA addresses.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_dma_alloc(struct qda_iommu_device *iommu_dev,
		  struct qda_gem_obj *gem_obj, size_t size);

/**
 * qda_dma_free() - Free DMA coherent memory for a GEM object
 * @gem_obj: Pointer to GEM object to free memory for
 *
 * Frees DMA-coherent memory previously allocated for the GEM object
 * and cleans up the GEM object fields.
 */
void qda_dma_free(struct qda_gem_obj *gem_obj);

/**
 * qda_dma_mmap() - Map DMA memory into userspace
 * @gem_obj: Pointer to GEM object containing DMA memory
 * @vma: Virtual memory area to map into
 *
 * Maps DMA-coherent memory into userspace virtual address space.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_dma_mmap(struct qda_gem_obj *gem_obj, struct vm_area_struct *vma);

#endif /* _QDA_MEMORY_DMA_H */
