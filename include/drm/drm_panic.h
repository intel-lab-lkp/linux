/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef __DRM_PANIC_H__
#define __DRM_PANIC_H__

/*
 * Copyright (c) 2023 Jocelyn Falempe <jfalempe@redhat.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/iosys-map.h>

struct drm_device;

struct drm_scanout_buffer {
	const struct drm_format_info *format;
	struct iosys_map map;
	unsigned int pitch;
	unsigned int width;
	unsigned int height;
};

#ifdef CONFIG_DRM_PANIC

void drm_panic_init(void);
void drm_panic_exit(void);

void drm_panic_register(struct drm_device *dev);
void drm_panic_unregister(struct drm_device *dev);

#else

static inline void drm_panic_init(void) {}
static inline void drm_panic_exit(void) {}

static inline void drm_panic_register(struct drm_device *dev) {}
static inline void drm_panic_unregister(struct drm_device *dev) {}

#endif

#endif /* __DRM_LOG_H__ */
