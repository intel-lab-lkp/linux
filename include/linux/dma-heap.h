/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMABUF Heaps Allocation Infrastructure
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#ifndef _DMA_HEAPS_H
#define _DMA_HEAPS_H

#include <linux/cdev.h>
#include <linux/types.h>

struct dma_heap;
struct dma_heap_file;
struct dma_heap_file_task;
struct dma_heap_file;

/**
 * struct dma_heap_ops - ops to operate on a given heap
 * @allocate:			allocate dmabuf and return struct dma_buf ptr
 * @allocate_async_read:	allocate and async read file.
 * allocate returns dmabuf on success, ERR_PTR(-errno) on error.
 */
struct dma_heap_ops {
	struct dma_buf *(*allocate)(struct dma_heap *heap,
				    unsigned long len,
				    u32 fd_flags,
				    u64 heap_flags);
	struct dma_buf *(*allocate_async_read)(struct dma_heap *heap,
					       struct dma_heap_file *heap_file,
					       u32 fd_flags, u64 heap_flags);
};

/**
 * struct dma_heap_export_info - information needed to export a new dmabuf heap
 * @name:	used for debugging/device-node name
 * @ops:	ops struct for this heap
 * @priv:	heap exporter private data
 *
 * Information needed to export a new dmabuf heap.
 */
struct dma_heap_export_info {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
};

/**
 * dma_heap_get_drvdata() - get per-heap driver data
 * @heap: DMA-Heap to retrieve private data for
 *
 * Returns:
 * The per-heap data for the heap.
 */
void *dma_heap_get_drvdata(struct dma_heap *heap);

/**
 * dma_heap_get_name() - get heap name
 * @heap: DMA-Heap to retrieve private data for
 *
 * Returns:
 * The char* for the heap name.
 */
const char *dma_heap_get_name(struct dma_heap *heap);

/**
 * dma_heap_add - adds a heap to dmabuf heaps
 * @exp_info:		information needed to register this heap
 */
struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info);

/**
 * dma_heap_wait_for_file_read - waits for a file read to complete
 *
 * Some users need to call this function before destroying the page to ensure
 * that all file work has been completed, in order to avoid UAF issues.
 * Remember, this function does not destroy the data structure corresponding to
 * the ftask. Before ending the actual processing, you need to call
 * @dma_heap_end_file_read.
 *
 * 0 - success, -EIO - if any file work failed
 */
int dma_heap_wait_for_file_read(struct dma_heap_file_task *heap_ftask);

/**
 * dma_heap_end_file_read - waits for a file read to complete then destroy it
 * 0 - success, -EIO - if any file work failed
 */
int dma_heap_end_file_read(struct dma_heap_file_task *heap_ftask);

/**
 * dma_heap_alloc_file_read - Declare a task to read file when allocate pages.
 * @heap_file:		target file to read
 *
 * Return NULL if failed, otherwise return a struct pointer.
 */
struct dma_heap_file_task *
dma_heap_declare_file_read(struct dma_heap_file *heap_file);

/**
 * dma_heap_gather_file_page - gather each allocated page.
 * @heap_ftask:		prepared and need to commit's work.
 * @page:		current allocated page. don't care which order.
 *
 * This function gather all allocated pages, automatically submit when the
 * gathering reaches the limit. Submit will package pages, prepare the data
 * required for reading file, then submit to async read thread.
 *
 * 0 - success, nagtive - failed.
 */
int dma_heap_gather_file_page(struct dma_heap_file_task *heap_ftask,
			      struct page *page);
size_t dma_heap_file_size(struct dma_heap_file *heap_file);

#endif /* _DMA_HEAPS_H */
