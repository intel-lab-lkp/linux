/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#ifndef _MA35_DRM_H_
#define _MA35_DRM_H_

#include <linux/regmap.h>
#include <linux/types.h>
#include <drm/drm_device.h>

#include "ma35_regs.h"
#include "ma35_plane.h"
#include "ma35_crtc.h"
#include "ma35_interface.h"

#define DRIVER_MAJOR	1
#define DRIVER_MINOR	0

#define MA35_INT_STATE_DISP0	BIT(0)

#define MA35_DISPLAY_ALIGN_PIXELS	32
#define MA35_DISPLAY_PREFER_DEPTH	32

#define MA35_CURSOR_WIDTH	32
#define MA35_CURSOR_HEIGHT	32

#define MA35_DISPLAY_MAX_ZPOS	3

#define ma35_drm(d) \
	container_of(d, struct ma35_drm, drm_dev)

struct ma35_drm {
	struct drm_device drm_dev;
	struct regmap *regmap;
	struct list_head layers_list;
	struct ma35_crtc *crtc;
	struct ma35_interface *interface;
	struct clk *dcuclk;
	struct clk *dcupclk;
};

#endif
