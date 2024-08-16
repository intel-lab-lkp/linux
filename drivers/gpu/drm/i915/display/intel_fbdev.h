/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2019 Intel Corporation
 */

#ifndef __INTEL_FBDEV_H__
#define __INTEL_FBDEV_H__

#include <linux/types.h>

struct drm_device;
struct drm_fb_helper;
struct drm_fb_helper_surface_size;
struct drm_i915_private;
struct intel_fbdev;
struct intel_framebuffer;

#ifdef CONFIG_DRM_FBDEV_EMULATION
int intel_fbdev_driver_fbdev_probe(struct drm_fb_helper *helper,
				   struct drm_fb_helper_surface_size *sizes);
#define INTEL_FBDEV_DRIVER_OPS \
	.fbdev_probe = intel_fbdev_driver_fbdev_probe
void intel_fbdev_setup(struct drm_i915_private *dev_priv);
void intel_fbdev_set_suspend(struct drm_device *dev, int state, bool synchronous);
struct intel_framebuffer *intel_fbdev_framebuffer(struct intel_fbdev *fbdev);
#else
#define INTEL_FBDEV_DRIVER_OPS \
	.fbdev_probe = NULL
static inline void intel_fbdev_setup(struct drm_i915_private *dev_priv)
{
}

static inline void intel_fbdev_set_suspend(struct drm_device *dev, int state, bool synchronous)
{
}

static inline struct intel_framebuffer *intel_fbdev_framebuffer(struct intel_fbdev *fbdev)
{
	return NULL;
}
#endif

#endif /* __INTEL_FBDEV_H__ */
