/* SPDX-License-Identifier: GPL-2.0 or MIT */
#ifndef __DRM_PANIC_H__
#define __DRM_PANIC_H__

/*
 * Copyright (c) 2023 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/iosys-map.h>

struct drm_plane;

#ifdef CONFIG_DRM_PANIC

void drm_panic_register(struct drm_plane *plane);
void drm_panic_unregister(struct drm_plane *plane);

void drm_panic_set_buffer(struct drm_scanout_buffer *sb,
			  struct drm_framebuffer *fb,
			  struct iosys_map *map);
void drm_panic_unset_buffer(struct drm_scanout_buffer *sb);
#else

static inline void drm_panic_register(struct drm_plane *plane) {}
static inline void drm_panic_unregister(struct drm_plane *plane) {}

static inline void drm_panic_set_buffer(struct drm_scanout_buffer *sb,
				 struct drm_framebuffer *fb,
				 struct iosys_map *map) {}
static inline void drm_panic_unset_buffer(struct drm_scanout_buffer *sb) {}

#endif

#endif /* __DRM_PANIC_H__ */
