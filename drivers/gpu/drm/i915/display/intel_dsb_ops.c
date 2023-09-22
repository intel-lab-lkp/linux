// SPDX-License-Identifier: MIT
/*
 * Copyright 2023, Intel Corporation.
 */

#include "gem/i915_gem_internal.h"
#include "i915_drv.h"
#include "i915_vma.h"
#include "intel_display_types.h"
#include "intel_dsb.h"

u32 intel_dsb_ggtt_offset(struct intel_dsb *dsb)
{
	return i915_ggtt_offset(dsb->vma);
}

void intel_dsb_write(struct intel_dsb *dsb, u32 idx, u32 val)
{
	dsb->cmd_buf[idx] = val;
}

u32 intel_dsb_read(struct intel_dsb *dsb, u32 idx)
{
	return dsb->cmd_buf[idx];
}

void intel_dsb_memset(struct intel_dsb *dsb, u32 idx, u32 val, u32 sz)
{
	memset(&dsb->cmd_buf[idx], val, sz);
}

bool intel_dsb_buffer_create(struct intel_crtc *crtc, struct intel_dsb *dsb, u32 size)
{
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	u32 *buf;

	obj = i915_gem_object_create_internal(i915, PAGE_ALIGN(size));
	if (IS_ERR(obj))
		return false;

	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0, 0);
	if (IS_ERR(vma)) {
		i915_gem_object_put(obj);
		return false;
	}

	buf = i915_gem_object_pin_map_unlocked(vma->obj, I915_MAP_WC);
	if (IS_ERR(buf)) {
		i915_vma_unpin_and_release(&vma, I915_VMA_RELEASE_MAP);
		return false;
	}

	dsb->id = DSB1;
	dsb->vma = vma;
	dsb->crtc = crtc;
	dsb->cmd_buf = buf;

	return true;
}

void intel_dsb_cleanup(struct intel_dsb *dsb)
{
	i915_vma_unpin_and_release(&dsb->vma, I915_VMA_RELEASE_MAP);
	kfree(dsb);
}
