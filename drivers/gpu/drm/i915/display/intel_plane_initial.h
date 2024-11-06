/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_PLANE_INITIAL_H__
#define __INTEL_PLANE_INITIAL_H__

struct drm_i915_private;

void intel_plane_initial_vblank_wait(struct intel_plane *plane);
void intel_initial_plane_config(struct drm_i915_private *i915);

#endif
