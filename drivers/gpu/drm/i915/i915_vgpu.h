/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2011-2015 Intel Corporation. All rights reserved.
 */

#ifndef _I915_VGPU_H_
#define _I915_VGPU_H_

#include <linux/types.h>

struct drm_i915_private;
struct i915_ggtt;

void intel_vgpu_detect(struct drm_i915_private *i915);
bool intel_vgpu_active(struct drm_i915_private *i915);
void intel_vgpu_register(struct drm_i915_private *i915);
bool intel_vgpu_has_full_ppgtt(struct drm_i915_private *i915);
bool intel_vgpu_has_hwsp_emulation(struct drm_i915_private *i915);
bool intel_vgpu_has_huge_gtt(struct drm_i915_private *i915);

int intel_vgt_balloon(struct i915_ggtt *ggtt);
void intel_vgt_deballoon(struct i915_ggtt *ggtt);

#endif /* _I915_VGPU_H_ */
