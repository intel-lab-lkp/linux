// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <drm/intel/display_parent_interface.h>

#include "regs/xe_gtt_defs.h"

/* FIXME move intel_remapped_info_size() & co. */
#include "intel_fb.h"

/* FIXME move intel_initial_plane_config */
#include "intel_display_types.h"

#include "xe_bo.h"
#include "xe_display_bo.h"
#include "xe_display_vma.h"
#include "xe_fb_pin.h"
#include "xe_ggtt.h"
#include "xe_mmio.h"
#include "xe_ttm_stolen_mgr.h"
#include "xe_vram_types.h"

static bool is_pte_local(u64 pte)
{
	return pte & XE_GGTT_PTE_DM;
}

static bool has_lmembar(struct xe_device *xe)
{
	return GRAPHICS_VERx100(xe) >= 1270;
}

static bool need_pte_local(struct xe_device *xe)
{
	return IS_DGFX(xe) || has_lmembar(xe);
}

static struct drm_gem_object *
xe_alloc_initial_plane_obj(struct drm_device *drm,
			   struct intel_initial_plane_config *plane_config)
{
	struct xe_device *xe = to_xe_device(drm);
	struct xe_tile *tile0 = xe_device_get_root_tile(xe);
	struct xe_bo *bo;
	resource_size_t phys_base;
	u32 base, size, flags;
	u64 page_size = xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K ? SZ_64K : SZ_4K;

	if (plane_config->size == 0)
		return NULL;

	flags = XE_BO_FLAG_FORCE_WC | XE_BO_FLAG_GGTT;

	base = round_down(plane_config->base, page_size);
	size = round_up(plane_config->base + plane_config->size,
			page_size);
	size -= base;

	if (IS_DGFX(xe)) {
		u64 pte = xe_ggtt_read_pte(tile0->mem.ggtt, base);

		if (is_pte_local(pte) != need_pte_local(xe)) {
			drm_err(&xe->drm, "Initial plane PTE has bad local memory bit\n");
			return NULL;
		}

		phys_base = pte & ~(page_size - 1);

		flags |= XE_BO_FLAG_VRAM0;

		/*
		 * We don't currently expect this to ever be placed in the
		 * stolen portion.
		 */
		if (phys_base >= xe_vram_region_usable_size(tile0->mem.vram)) {
			drm_err(&xe->drm,
				"Initial plane programming using invalid range, phys_base=%pa\n",
				&phys_base);
			return NULL;
		}

		drm_dbg_kms(&xe->drm,
			    "Using phys_base=%pa, based on initial plane programming\n",
			    &phys_base);
	} else {
		struct ttm_resource_manager *stolen;
		u64 pte;

		stolen = ttm_manager_type(&xe->ttm, XE_PL_STOLEN);
		if (!stolen) {
			drm_dbg_kms(&xe->drm, "No stolen for initial FB\n");
			return NULL;
		}

		pte = xe_ggtt_read_pte(tile0->mem.ggtt, base);

		if (is_pte_local(pte) != need_pte_local(xe)) {
			drm_err(&xe->drm, "Initial plane PTE has bad local memory bit\n");
			return NULL;
		}

		phys_base = base;
		flags |= XE_BO_FLAG_STOLEN;

		if (IS_ENABLED(CONFIG_FRAMEBUFFER_CONSOLE) &&
		    IS_ENABLED(CONFIG_DRM_FBDEV_EMULATION) &&
		    !xe_display_bo_fbdev_prefer_stolen(xe, plane_config->size)) {
			drm_info(&xe->drm, "Initial FB size exceeds half of stolen, discarding\n");
			return NULL;
		}
	}

	bo = xe_bo_create_pin_map_at_novm(xe, tile0, size, phys_base,
					  ttm_bo_type_kernel, flags, 0, false);
	if (IS_ERR(bo)) {
		drm_dbg_kms(&xe->drm,
			    "Failed to create bo phys_base=%pa size %u with flags %x: %li\n",
			    &phys_base, size, flags, PTR_ERR(bo));
		return NULL;
	}

	return &bo->ttm.base;
}

static void
xe_free_initial_plane_obj(struct drm_gem_object *obj)
{
	xe_bo_unpin_map_no_vm(gem_to_xe_bo(obj));
}

const struct intel_display_initial_plane_interface xe_display_initial_plane_interface = {
	.alloc_obj = xe_alloc_initial_plane_obj,
	.free_obj = xe_free_initial_plane_obj,
};
