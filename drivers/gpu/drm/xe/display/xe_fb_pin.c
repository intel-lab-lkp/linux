// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <drm/ttm/ttm_bo.h>

#include "intel_display_types.h"
#include "intel_dpt.h"
#include "intel_fb.h"
#include "intel_fb_pin.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_ggtt.h"
#include "xe_pm.h"

static void encode_and_write_pte(struct xe_bo *bo, struct iosys_map *map,
				 u32 *ofs, u32 src_idx, struct xe_device *xe)
{
	struct xe_ggtt *ggtt = xe_device_get_root_tile(xe)->mem.ggtt;
	u64 pte = ggtt->pt_ops->pte_encode_bo(bo, src_idx * XE_PAGE_SIZE,
					      xe->pat.idx[XE_CACHE_NONE]);
	iosys_map_wr(map, *ofs, u64, pte);
	*ofs += 8;
}

static void write_dpt(struct xe_bo *bo, struct iosys_map *map, u32 *dpt_ofs,
		      const struct intel_remapped_plane_info *plane,
		      enum i915_gtt_view_type type)
{
	struct xe_device *xe = xe_bo_device(bo);
	const u32 dpt_even = (plane->dst_stride - (type == I915_GTT_VIEW_ROTATED
			      ? plane->height : plane->width)) * 8;
	const u32 dest_width = type == I915_GTT_VIEW_ROTATED
		? plane->height : plane->width;
	const u32 dest_height = type == I915_GTT_VIEW_ROTATED
		? plane->width : plane->height;
	u32 src_idx;

	for (u32 row = 0; row < dest_height; ++row) {
		for (u32 column = 0; column < dest_width; ++column) {
			switch (type) {
			case I915_GTT_VIEW_NORMAL:
				src_idx = plane->offset + column;
				break;
			case I915_GTT_VIEW_REMAPPED:
				src_idx = plane->offset +
					row * plane->src_stride + column;
				break;
			case I915_GTT_VIEW_ROTATED:
				src_idx = plane->offset
					+ plane->src_stride
					* (plane->height - 1)
					- column * plane->src_stride
					+ row;
				break;
			}
			encode_and_write_pte(bo, map, dpt_ofs, src_idx, xe);
		}
		*dpt_ofs += dpt_even;
	}
	*dpt_ofs = ALIGN(*dpt_ofs, 4096);
}

static int __xe_pin_fb_vma_dpt(const struct intel_framebuffer *fb,
			       const struct i915_gtt_view *view,
			       struct i915_vma *vma,
			       u64 physical_alignment)
{
	struct xe_device *xe = to_xe_device(fb->base.dev);
	struct xe_tile *tile0 = xe_device_get_root_tile(xe);
	struct drm_gem_object *obj = intel_fb_bo(&fb->base);
	struct xe_bo *bo = gem_to_xe_bo(obj), *dpt;
	u32 dpt_size, size = bo->ttm.base.size;
	const struct intel_remapped_plane_info *plane;
	u32 i, plane_count, dpt_ofs = 0;
	struct intel_remapped_plane_info normal_plane;

	if (view->type == I915_GTT_VIEW_NORMAL) {
		dpt_size = ALIGN(size / XE_PAGE_SIZE * 8, XE_PAGE_SIZE);
		normal_plane.offset = 0;
		normal_plane.width = size / XE_PAGE_SIZE;
		normal_plane.height = 1;
		normal_plane.src_stride = size / XE_PAGE_SIZE;
		normal_plane.dst_stride = size / XE_PAGE_SIZE;
		plane = &normal_plane;
		plane_count = 1;
	} else if (view->type == I915_GTT_VIEW_REMAPPED) {
		dpt_size = ALIGN(intel_remapped_info_size(&view->remapped) * 8,
				 XE_PAGE_SIZE);
		plane = view->remapped.plane;
		plane_count = ARRAY_SIZE(view->remapped.plane);
	} else {
		/* display uses 4K tiles instead of bytes here, convert to entries.. */
		dpt_size = ALIGN(intel_rotation_info_size(&view->rotated) * 8,
				 XE_PAGE_SIZE);
		plane = view->rotated.plane;
		plane_count = ARRAY_SIZE(view->rotated.plane);
	}

	if (IS_DGFX(xe))
		dpt = xe_bo_create_pin_map_at_aligned(xe, tile0, NULL,
						      dpt_size, ~0ull,
						      ttm_bo_type_kernel,
						      XE_BO_FLAG_VRAM0 |
						      XE_BO_FLAG_GGTT |
						      XE_BO_FLAG_PAGETABLE,
						      physical_alignment);
	else
		dpt = xe_bo_create_pin_map_at_aligned(xe, tile0, NULL,
						      dpt_size,  ~0ull,
						      ttm_bo_type_kernel,
						      XE_BO_FLAG_STOLEN |
						      XE_BO_FLAG_GGTT |
						      XE_BO_FLAG_PAGETABLE,
						      physical_alignment);
	if (IS_ERR(dpt))
		dpt = xe_bo_create_pin_map_at_aligned(xe, tile0, NULL,
						      dpt_size,  ~0ull,
						      ttm_bo_type_kernel,
						      XE_BO_FLAG_SYSTEM |
						      XE_BO_FLAG_GGTT |
						      XE_BO_FLAG_PAGETABLE,
						      physical_alignment);
	if (IS_ERR(dpt))
		return PTR_ERR(dpt);

	for (i = 0; i < plane_count; i++)
		write_dpt(bo, &dpt->vmap, &dpt_ofs, &plane[i], view->type);

	vma->dpt = dpt;
	vma->node = dpt->ggtt_node[tile0->id];
	return 0;
}

static void
write_ggtt_rotated(struct xe_bo *bo, struct xe_ggtt *ggtt, u32 *ggtt_ofs, u32 bo_ofs,
		   u32 width, u32 height, u32 src_stride, u32 dst_stride)
{
	struct xe_device *xe = xe_bo_device(bo);
	u32 column, row;

	for (column = 0; column < width; column++) {
		u32 src_idx = src_stride * (height - 1) + column + bo_ofs;

		for (row = 0; row < height; row++) {
			u64 pte = ggtt->pt_ops->pte_encode_bo(bo, src_idx * XE_PAGE_SIZE,
							      xe->pat.idx[XE_CACHE_NONE]);

			ggtt->pt_ops->ggtt_set_pte(ggtt, *ggtt_ofs, pte);
			*ggtt_ofs += XE_PAGE_SIZE;
			src_idx -= src_stride;
		}

		/* The DE ignores the PTEs for the padding tiles */
		*ggtt_ofs += (dst_stride - height) * XE_PAGE_SIZE;
	}
}

static int __xe_pin_fb_vma_ggtt(const struct intel_framebuffer *fb,
				const struct i915_gtt_view *view,
				struct i915_vma *vma,
				u64 physical_alignment)
{
	struct drm_gem_object *obj = intel_fb_bo(&fb->base);
	struct xe_bo *bo = gem_to_xe_bo(obj);
	struct xe_device *xe = to_xe_device(fb->base.dev);
	struct xe_ggtt *ggtt = xe_device_get_root_tile(xe)->mem.ggtt;
	u32 align;
	int ret;

	/* TODO: Consider sharing framebuffer mapping?
	 * embed i915_vma inside intel_framebuffer
	 */
	xe_pm_runtime_get_noresume(tile_to_xe(ggtt->tile));
	ret = mutex_lock_interruptible(&ggtt->lock);
	if (ret)
		goto out;

	align = XE_PAGE_SIZE;
	if (xe_bo_is_vram(bo) && ggtt->flags & XE_GGTT_FLAGS_64K)
		align = max_t(u32, align, SZ_64K);

	if (bo->ggtt_node[ggtt->tile->id] && view->type == I915_GTT_VIEW_NORMAL) {
		vma->node = bo->ggtt_node[ggtt->tile->id];
	} else if (view->type == I915_GTT_VIEW_NORMAL) {
		u32 x, size = bo->ttm.base.size;

		vma->node = xe_ggtt_node_init(ggtt);
		if (IS_ERR(vma->node)) {
			ret = PTR_ERR(vma->node);
			goto out_unlock;
		}

		ret = xe_ggtt_node_insert_locked(vma->node, size, align, 0);
		if (ret) {
			xe_ggtt_node_fini(vma->node);
			goto out_unlock;
		}

		for (x = 0; x < size; x += XE_PAGE_SIZE) {
			u64 pte = ggtt->pt_ops->pte_encode_bo(bo, x,
							      xe->pat.idx[XE_CACHE_NONE]);

			ggtt->pt_ops->ggtt_set_pte(ggtt, vma->node->base.start + x, pte);
		}
	} else {
		u32 i, ggtt_ofs;
		const struct intel_rotation_info *rot_info = &view->rotated;

		/* display seems to use tiles instead of bytes here, so convert it back.. */
		u32 size = intel_rotation_info_size(rot_info) * XE_PAGE_SIZE;

		vma->node = xe_ggtt_node_init(ggtt);
		if (IS_ERR(vma->node)) {
			ret = PTR_ERR(vma->node);
			goto out_unlock;
		}

		ret = xe_ggtt_node_insert_locked(vma->node, size, align, 0);
		if (ret) {
			xe_ggtt_node_fini(vma->node);
			goto out_unlock;
		}

		ggtt_ofs = vma->node->base.start;

		for (i = 0; i < ARRAY_SIZE(rot_info->plane); i++)
			write_ggtt_rotated(bo, ggtt, &ggtt_ofs,
					   rot_info->plane[i].offset,
					   rot_info->plane[i].width,
					   rot_info->plane[i].height,
					   rot_info->plane[i].src_stride,
					   rot_info->plane[i].dst_stride);
	}

out_unlock:
	mutex_unlock(&ggtt->lock);
out:
	xe_pm_runtime_put(tile_to_xe(ggtt->tile));
	return ret;
}

static struct i915_vma *__xe_pin_fb_vma(const struct intel_framebuffer *fb,
					const struct i915_gtt_view *view,
					u64 physical_alignment)
{
	struct drm_device *dev = fb->base.dev;
	struct xe_device *xe = to_xe_device(dev);
	struct i915_vma *vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	struct drm_gem_object *obj = intel_fb_bo(&fb->base);
	struct xe_bo *bo = gem_to_xe_bo(obj);
	int ret;

	if (!vma)
		return ERR_PTR(-ENODEV);

	if (IS_DGFX(to_xe_device(bo->ttm.base.dev)) &&
	    intel_fb_rc_ccs_cc_plane(&fb->base) >= 0 &&
	    !(bo->flags & XE_BO_FLAG_NEEDS_CPU_ACCESS)) {
		struct xe_tile *tile = xe_device_get_root_tile(xe);

		/*
		 * If we need to able to access the clear-color value stored in
		 * the buffer, then we require that such buffers are also CPU
		 * accessible.  This is important on small-bar systems where
		 * only some subset of VRAM is CPU accessible.
		 */
		if (tile->mem.vram.io_size < tile->mem.vram.usable_size) {
			ret = -EINVAL;
			goto err;
		}
	}

	/*
	 * Pin the framebuffer, we can't use xe_bo_(un)pin functions as the
	 * assumptions are incorrect for framebuffers
	 */
	ret = ttm_bo_reserve(&bo->ttm, false, false, NULL);
	if (ret)
		goto err;

	if (IS_DGFX(xe))
		ret = xe_bo_migrate(bo, XE_PL_VRAM0);
	else
		ret = xe_bo_validate(bo, NULL, true);
	if (!ret)
		ttm_bo_pin(&bo->ttm);
	ttm_bo_unreserve(&bo->ttm);
	if (ret)
		goto err;

	vma->bo = bo;
	if (intel_fb_uses_dpt(&fb->base))
		ret = __xe_pin_fb_vma_dpt(fb, view, vma, physical_alignment);
	else
		ret = __xe_pin_fb_vma_ggtt(fb, view, vma,  physical_alignment);
	if (ret)
		goto err_unpin;

	/* Ensure DPT writes are flushed */
	xe_device_l2_flush(xe);
	return vma;

err_unpin:
	ttm_bo_reserve(&bo->ttm, false, false, NULL);
	ttm_bo_unpin(&bo->ttm);
	ttm_bo_unreserve(&bo->ttm);
err:
	kfree(vma);
	return ERR_PTR(ret);
}

static void __xe_unpin_fb_vma(struct i915_vma *vma)
{
	u8 tile_id = vma->node->ggtt->tile->id;

	if (vma->dpt)
		xe_bo_unpin_map_no_vm(vma->dpt);
	else if (!xe_ggtt_node_allocated(vma->bo->ggtt_node[tile_id]) ||
		 vma->bo->ggtt_node[tile_id]->base.start != vma->node->base.start)
		xe_ggtt_node_remove(vma->node, false);

	ttm_bo_reserve(&vma->bo->ttm, false, false, NULL);
	ttm_bo_unpin(&vma->bo->ttm);
	ttm_bo_unreserve(&vma->bo->ttm);
	kfree(vma);
}

struct i915_vma *
intel_fb_pin_to_ggtt(const struct drm_framebuffer *fb,
		     const struct i915_gtt_view *view,
		     unsigned int alignment,
		     unsigned int phys_alignment,
		     bool uses_fence,
		     unsigned long *out_flags)
{
	*out_flags = 0;

	return __xe_pin_fb_vma(to_intel_framebuffer(fb), view, phys_alignment);
}

void intel_fb_unpin_vma(struct i915_vma *vma, unsigned long flags)
{
	__xe_unpin_fb_vma(vma);
}

int intel_plane_pin_fb(struct intel_plane_state *plane_state)
{
	struct drm_framebuffer *fb = plane_state->hw.fb;
	struct drm_gem_object *obj = intel_fb_bo(fb);
	struct xe_bo *bo = gem_to_xe_bo(obj);
	struct i915_vma *vma;
	struct intel_framebuffer *intel_fb = to_intel_framebuffer(fb);
	struct intel_plane *plane = to_intel_plane(plane_state->uapi.plane);
	u64 phys_alignment = plane->min_alignment(plane, fb, 0);

	/* We reject creating !SCANOUT fb's, so this is weird.. */
	drm_WARN_ON(bo->ttm.base.dev, !(bo->flags & XE_BO_FLAG_SCANOUT));

	vma = __xe_pin_fb_vma(intel_fb, &plane_state->view.gtt, phys_alignment);

	if (IS_ERR(vma))
		return PTR_ERR(vma);

	plane_state->ggtt_vma = vma;
	return 0;
}

void intel_plane_unpin_fb(struct intel_plane_state *old_plane_state)
{
	__xe_unpin_fb_vma(old_plane_state->ggtt_vma);
	old_plane_state->ggtt_vma = NULL;
}

/*
 * For Xe introduce dummy intel_dpt_create which just return NULL,
 * intel_dpt_destroy which does nothing, and fake intel_dpt_ofsset returning 0;
 */
struct i915_address_space *intel_dpt_create(struct intel_framebuffer *fb)
{
	return NULL;
}

void intel_dpt_destroy(struct i915_address_space *vm)
{
	return;
}

u64 intel_dpt_offset(struct i915_vma *dpt_vma)
{
	return 0;
}
