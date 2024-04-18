/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __XE_FB_PIN_H__
#define __XE_FB_PIN_H__

#include <linux/types.h>

struct i915_vma;
struct drm_framebuffer;
struct i915_gtt_view;

struct i915_vma *
xe_pin_and_fence_fb_obj_initial(struct drm_framebuffer *fb,
				const struct i915_gtt_view *view,
				u64 ggtt_start);

#endif
