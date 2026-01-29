/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#ifndef _MA35_INTERFACE_H_
#define _MA35_INTERFACE_H_

#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>

struct ma35_drm;

struct ma35_interface {
	struct drm_encoder drm_encoder;
	struct drm_connector drm_connector;
	struct drm_panel *drm_panel;
	struct drm_bridge *drm_bridge;

	u32 bus_flags;
};

int ma35_interface_init(struct ma35_drm *priv);

#endif
