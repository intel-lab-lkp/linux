/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_TILE_H_
#define _XE_TILE_H_

#include "xe_device_types.h"

struct xe_tile;

#if IS_ENABLED(CONFIG_DRM_XE_DEVMEM_MIRROR)
/**
 * xe_tile_from_dpagemap - Find xe_tile from drm_pagemap
 * @dpagemap: pointer to struct drm_pagemap
 *
 * Return: Pointer to xe_tile
 */
static inline struct xe_tile *xe_tile_from_dpagemap(struct drm_pagemap *dpagemap)
{
	return container_of(dpagemap, struct xe_tile, mem.vram.dpagemap);
}

#else
static inline  struct xe_tile *xe_tile_from_dpagemap(struct drm_pagemap *dpagemap)
{
	return NULL;
}
#endif
int xe_tile_init_early(struct xe_tile *tile, struct xe_device *xe, u8 id);
int xe_tile_init_noalloc(struct xe_tile *tile);
int xe_tile_init(struct xe_tile *tile);

void xe_tile_migrate_wait(struct xe_tile *tile);

#endif
