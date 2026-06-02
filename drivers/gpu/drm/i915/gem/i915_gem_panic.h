/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Intel Corporation */

#ifndef __I915_GEM_PANIC_H__
#define __I915_GEM_PANIC_H__

#include <linux/types.h>

struct drm_gem_object;
struct drm_scanout_buffer;
struct intel_panic;

struct intel_panic *i915_gem_object_alloc_panic(void);
int i915_gem_object_panic_setup(struct intel_panic *panic, struct drm_scanout_buffer *sb,
				struct drm_gem_object *_obj, bool panic_tiling);
void i915_gem_object_panic_finish(struct intel_panic *panic);

#endif /* __I915_GEM_PANIC_H__ */
