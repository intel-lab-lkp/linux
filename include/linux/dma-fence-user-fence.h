/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __LINUX_DMA_FENCE_USER_FENCE_H
#define __LINUX_DMA_FENCE_USER_FENCE_H

#include <linux/dma-fence.h>
#include <linux/iosys-map.h>

/** struct dma_fence_user_fence - User fence */
struct dma_fence_user_fence {
	/** @cb: dma-fence callback used to attach user fence to dma-fence */
	struct dma_fence_cb cb;
	/** @map: IOSYS map to write seqno to */
	struct iosys_map map;
	/** @seqno: seqno to write to IOSYS map */
	u64 seqno;
};

struct dma_fence_user_fence *dma_fence_user_fence_alloc(void);

void dma_fence_user_fence_free(struct dma_fence_user_fence *user_fence);

void dma_fence_user_fence_attach(struct dma_fence *fence,
				 struct dma_fence_user_fence *user_fence,
				 struct iosys_map *map,
				 u64 seqno);

#endif
