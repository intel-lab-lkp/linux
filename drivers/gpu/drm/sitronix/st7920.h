/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Header file for:
 * DRM driver for Sitronix ST7920 LCD displays
 *
 * Copyright 2025 Iker Pedrosa <ikerpedrosam@gmail.com>
 *
 * Based on drivers/video/fbdev/ssd130x.c
 * Copyright 2022 Red Hat Inc.
 */

#ifndef __ST7920_H__
#define __ST7920_H__

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>

enum st7920_family_ids {
	ST7920_FAMILY
};

enum st7920_variants {
	/* st7920 family */
	ST7920_ID
};

struct st7920_deviceinfo {
	u32 default_dclk_div;
	u32 default_dclk_frq;
	u32 default_width;
	u32 default_height;

	enum st7920_family_ids family_id;
};

struct st7920_device {
	struct drm_device drm;
	struct device *dev;
	struct drm_display_mode mode;
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct spi_device *spi;

	struct regmap *regmap;

	const struct st7920_deviceinfo *device_info;

	u32 height;
	u32 width;
};

#endif /* __ST7920_H__ */
