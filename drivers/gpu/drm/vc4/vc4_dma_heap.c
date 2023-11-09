// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright © 2023 Simon Ser
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "vc4_drv.h"

static struct dma_buf *vc4_dma_heap_allocate(struct dma_heap *heap,
					     unsigned long size,
					     unsigned long fd_flags,
					     unsigned long heap_flags)
{
	struct vc4_dev *vc4 = dma_heap_get_drvdata(heap);
	//DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct drm_gem_dma_object *dma_obj;
	struct dma_buf *dmabuf;

	if (WARN_ON_ONCE(!vc4->is_vc5))
		return ERR_PTR(-ENODEV);

	dma_obj = drm_gem_dma_create(&vc4->base, size);
	if (IS_ERR(dma_obj))
		return ERR_CAST(dma_obj);

	dmabuf = drm_gem_prime_export(&dma_obj->base, fd_flags);
	drm_gem_object_put(&dma_obj->base);
	return dmabuf;
}

static const struct dma_heap_ops vc4_dma_heap_ops = {
	.allocate = vc4_dma_heap_allocate,
};

int vc4_dma_heap_create(struct vc4_dev *vc4)
{
	struct dma_heap_export_info exp_info;
	struct dma_heap *heap;

	exp_info.name = "vc4"; /* TODO: allow multiple? */
	exp_info.ops = &vc4_dma_heap_ops;
	exp_info.priv = vc4; /* TODO: unregister when unloading */

	heap = dma_heap_add(&exp_info);
	if (IS_ERR(heap))
		return PTR_ERR(heap);

	return 0;
}
