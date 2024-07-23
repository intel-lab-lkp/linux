/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef INTEL_ENGINE_SYSFS_H
#define INTEL_ENGINE_SYSFS_H

struct drm_i915_private;

void intel_engines_add_sysfs(struct drm_i915_private *i915);
void intel_engines_remove_sysfs(struct drm_i915_private *i915);

#endif /* INTEL_ENGINE_SYSFS_H */
