/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_SYNC_TYPES_H_
#define _XE_SYNC_TYPES_H_

#include <linux/types.h>

struct drm_syncobj;
struct dma_fence;
struct dma_fence_chain;
struct dma_fence_user_fence;
struct user_fence;
struct xe_bo;

struct xe_sync_entry {
	struct drm_syncobj *syncobj;
	struct dma_fence *fence;
	struct dma_fence_chain *chain_fence;
	struct xe_user_fence *ufence;
	struct dma_fence_user_fence *user_fence;
	struct xe_bo *bo;
	u64 addr;
	u64 timeline_value;
	u32 type;
	u32 flags;
};

#endif
