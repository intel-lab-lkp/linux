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

/**
 * struct drm_scanout_buffer - DRM scanout buffer
 *
 * This structure holds the information necessary for drm_panic to draw the
 * panic screen, and display it.
 * If the driver can't provide a linear buffer, it must clear @map with
 * iosys_map_clear() and provide a draw_pixel_xy() function.
 */
struct drm_scanout_buffer {
	/**
	 * @format:
	 *
	 * drm format of the scanout buffer.
	 */
	const struct drm_format_info *format;
	/**
	 * @map:
	 *
	 * Virtual address of the scanout buffer, either in memory or iomem.
	 * The scanout buffer should be in linear format, and can be directly
	 * sent to the display hardware. Tearing is not an issue for the panic
	 * screen.
	 */
	struct iosys_map map;
	/**
	 * @width: Width of the scanout buffer, in pixels.
	 */
	unsigned int width;
	/**
	 * @height: Height of the scanout buffer, in pixels.
	 */
	unsigned int height;
	/**
	 * @pitch: Length in bytes between the start of two consecutive lines.
	 */
	unsigned int pitch;
	/**
	 * @private:
	 *
	 * In case the driver can't provide a linear buffer, this is a pointer to
	 * some private data, that will be passed when calling @draw_pixel_xy()
	 * and @flush()
	 */
	void *private;
	/**
	 * @draw_pixel_xy:
	 *
	 * In case the driver can't provide a linear buffer, this is a function
	 * that drm_panic will call for each pixel to draw.
	 * Color will be converted to the format specified by @format.
	 */
	void (*draw_pixel_xy)(unsigned int x, unsigned int y, u32 color, void *private);
	/**
	 * @flush:
	 *
	 * This function is called after the panic screen is drawn, either using
	 * the iosys_map or the draw_pixel_xy path. In this function, the driver
	 * can send additional commands to the hardware, to make the buffer
	 * visible.
	 */
	void (*flush)(void *private);
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
