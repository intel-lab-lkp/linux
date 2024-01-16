/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _I915_GEM_OBJECT_H_
#define _I915_GEM_OBJECT_H_

#include <linux/types.h>
#include <linux/iosys-map.h>

#include "xe_bo.h"

#define i915_gem_object_is_shmem(obj) ((obj)->flags & XE_BO_CREATE_SYSTEM_BIT)

static inline dma_addr_t i915_gem_object_get_dma_address(const struct xe_bo *bo, pgoff_t n)
{
	/* Should never be called */
	WARN_ON(1);
	return n;
}

static inline bool i915_gem_object_is_tiled(const struct xe_bo *bo)
{
	/* legacy tiling is unused */
	return false;
}

static inline bool i915_gem_object_is_userptr(const struct xe_bo *bo)
{
	/* legacy tiling is unused */
	return false;
}

static inline int i915_gem_object_read_from_page(struct xe_bo *bo,
					  u32 ofs, u64 *ptr, u32 size)
{
	struct ttm_bo_kmap_obj map;
	void *virtual;
	struct iosys_map vaddr;
	bool is_iomem;
	int ret;

	XE_WARN_ON(size != 8);

	ret = xe_bo_lock(bo, true);
	if (ret)
		return ret;

	ret = ttm_bo_kmap(&bo->ttm, ofs >> PAGE_SHIFT, 1, &map);
	if (ret)
		goto out_unlock;

	ofs &= ~PAGE_MASK;
	virtual = ttm_kmap_obj_virtual(&map, &is_iomem);
	if (is_iomem)
		iosys_map_set_vaddr_iomem(&vaddr, (void __iomem *)(virtual));
	else
		iosys_map_set_vaddr(&vaddr, virtual);

	*ptr = iosys_map_rd(&vaddr, ofs, u64);
	ttm_bo_kunmap(&map);
out_unlock:
	xe_bo_unlock(bo);
	return ret;
}

#endif
