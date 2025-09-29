// SPDX-License-Identifier: MIT
/*
 * Copyright © 2017 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_gemfs.h"
#include "i915_utils.h"

void i915_gemfs_init(struct drm_i915_private *i915)
{
	struct vfsmount *gemfs;

	/*
	 * By creating our own shmemfs mountpoint, we can pass in
	 * mount flags that better match our usecase.
	 *
	 * One example, although it is probably better with a per-file
	 * control, is selecting huge page allocations ("huge=within_size").
	 * However, we only do so on platforms which benefit from it, or to
	 * offset the overhead of iommu lookups, where with latter it is a net
	 * win even on platforms which would otherwise see some performance
	 * regressions such a slow reads issue on Broadwell and Skylake.
	 */

	if (GRAPHICS_VER(i915) < 11 && !i915_vtd_active(i915))
		return;

	gemfs = drm_gem_shmem_huge_mnt_create("within_size");
	if (IS_ERR(gemfs))
		goto err;

	i915->mm.gemfs = gemfs;
	drm_info(&i915->drm, "Using Transparent Hugepages\n");
	return;

err:
	drm_notice(&i915->drm,
		   "Transparent Hugepage support is recommended for optimal performance%s\n",
		   GRAPHICS_VER(i915) >= 11 ? " on this platform!" :
					      " when IOMMU is enabled!");
}

void i915_gemfs_fini(struct drm_i915_private *i915)
{
	drm_gem_shmem_huge_mnt_free(i915->mm.gemfs);
}
