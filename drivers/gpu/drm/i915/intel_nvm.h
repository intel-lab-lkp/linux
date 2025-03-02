/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2019-2024 Intel Corporation. All rights reserved.
 */

#ifndef __INTEL_NVM_H__
#define __INTEL_NVM_H__

struct drm_i915_private;

void intel_nvm_init(struct drm_i915_private *i915);

void intel_nvm_fini(struct drm_i915_private *i915);

#endif /* __INTEL_NVM_H__ */
