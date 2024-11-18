// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/dma-fence-user-fence.h>
#include <linux/slab.h>

static void user_fence_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct dma_fence_user_fence *user_fence =
		container_of(cb, struct dma_fence_user_fence, cb);

	if (user_fence->map.is_iomem)
		writeq(user_fence->seqno, user_fence->map.vaddr_iomem);
	else
		*(u64 *)user_fence->map.vaddr = user_fence->seqno;

	dma_fence_user_fence_free(user_fence);
}

/**
 * dma_fence_user_fence_alloc() - Allocate user fence
 *
 * Return: Allocated struct dma_fence_user_fence on Success, NULL on failure
 */
struct dma_fence_user_fence *dma_fence_user_fence_alloc(void)
{
	return kmalloc(sizeof(struct dma_fence_user_fence), GFP_KERNEL);
}
EXPORT_SYMBOL(dma_fence_user_fence_alloc);

/**
 * dma_fence_user_fence_free() - Free user fence
 *
 * Free user fence. Should only be called on a user fence if
 * dma_fence_user_fence_attach is not called to cleanup original allocation from
 * dma_fence_user_fence_alloc.
 */
void dma_fence_user_fence_free(struct dma_fence_user_fence *user_fence)
{
	kfree(user_fence);
}
EXPORT_SYMBOL(dma_fence_user_fence_free);

/**
 * dma_fence_user_fence_attach() - Attach user fence to dma-fence
 *
 * @fence: fence
 * @user_fence user fence
 * @map: IOSYS map to write seqno to
 * @seqno: seqno to write to IOSYS map
 *
 * Attach a user fence, which is a seqno write to an IOSYS map, to a DMA fence.
 * The caller must guarantee that the memory in the IOSYS map doesn't move
 * before the fence signals. This is typically done by installing the DMA fence
 * into the BO's DMA reservation bookkeeping slot from which the IOSYS was
 * derived.
 */
void dma_fence_user_fence_attach(struct dma_fence *fence,
				 struct dma_fence_user_fence *user_fence,
				 struct iosys_map *map, u64 seqno)
{
	int err;

	user_fence->map = *map;
	user_fence->seqno = seqno;

	err = dma_fence_add_callback(fence, &user_fence->cb, user_fence_cb);
	if (err == -ENOENT)
		user_fence_cb(NULL, &user_fence->cb);
}
EXPORT_SYMBOL(dma_fence_user_fence_attach);
