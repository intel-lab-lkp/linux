/*
 * Copyright © 2017 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vc4_drv.h"

struct dma_fence *vc4_fence_create(struct vc4_dev *vc4, enum vc4_queue q)
{
	struct vc4_queue_state *queue = &vc4->queue[q];
	struct vc4_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	fence->dev = &vc4->base;
	fence->queue = q;
	fence->seqno = ++queue->emit_seqno;
	dma_fence_init(&fence->base, &vc4_fence_ops, NULL, queue->fence_context,
		       fence->seqno);

	return &fence->base;
}

static const char *vc4_fence_get_driver_name(struct dma_fence *fence)
{
	return "vc4";
}

static const char *vc4_fence_get_timeline_name(struct dma_fence *fence)
{
	return "vc4-v3d";
}

const struct dma_fence_ops vc4_fence_ops = {
	.get_driver_name = vc4_fence_get_driver_name,
	.get_timeline_name = vc4_fence_get_timeline_name,
};
