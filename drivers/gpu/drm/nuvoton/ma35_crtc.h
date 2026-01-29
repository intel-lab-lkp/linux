/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#ifndef _MA35_CRTC_H_
#define _MA35_CRTC_H_

#include <drm/drm_crtc.h>

struct drm_pending_vblank_event;
struct ma35_drm;

#define MA35_DPI_D24	5
#define MA35_DPI_FORMAT_MASK GENMASK(2, 0)

struct ma35_crtc {
	struct drm_crtc drm_crtc;
	atomic_t vblank_counter;
	u32 dpi_format;
	u16 dither_depth;
	bool dither_enable;
};

#define MA35_DEFAULT_CRTC_ID	0

#define MA35_MAX_PIXEL_CLK		150000

#define MA35_GAMMA_TABLE_SIZE	256
#define MA35_GAMMA_RED_MASK		GENMASK(29, 20)
#define MA35_GAMMA_GREEN_MASK	GENMASK(19, 10)
#define MA35_GAMMA_BLUE_MASK	GENMASK(9, 0)

#define MA35_DITHER_TABLE_ENTRY	16
#define MA35_DITHER_ENABLE		BIT(31)
#define MA35_DITHER_TABLE_MASK	GENMASK(3, 0)

#define MA35_CRTC_VBLANK		BIT(0)

#define MA35_DEBUG_COUNTER_MASK		GENMASK(31, 0)

#define MA35_PANEL_DATA_ENABLE_ENABLE	BIT(0)
#define MA35_PANEL_DATA_ENABLE_POLARITY	BIT(1)
#define MA35_PANEL_DATA_ENABLE			BIT(4)
#define MA35_PANEL_DATA_POLARITY		BIT(5)
#define MA35_PANEL_DATA_CLOCK_ENABLE	BIT(8)
#define MA35_PANEL_DATA_CLOCK_POLARITY	BIT(9)

#define MA35_DISPLAY_TOTAL_MASK		GENMASK(30, 16)
#define MA35_DISPLAY_ACTIVE_MASK	GENMASK(14, 0)

#define MA35_SYNC_POLARITY_BIT	BIT(31)
#define MA35_SYNC_PULSE_ENABLE	BIT(30)
#define MA35_SYNC_END_MASK		GENMASK(29, 15)
#define MA35_SYNC_START_MASK	GENMASK(14, 0)

#define MA35_DISPLAY_CURRENT_X	GENMASK(15, 0)
#define MA35_DISPLAY_CURRENT_Y	GENMASK(31, 16)

void ma35_crtc_vblank_handler(struct ma35_drm *priv);
int ma35_crtc_init(struct ma35_drm *priv);

#endif
