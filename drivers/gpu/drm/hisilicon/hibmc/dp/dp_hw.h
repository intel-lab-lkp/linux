/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_KAPI_H
#define DP_KAPI_H

#include <linux/types.h>
#include <linux/delay.h>

#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_print.h>
#include <video/videomode.h>

struct dp_dev;

struct hibmc_dp {
	struct dp_dev *dp_dev;
	struct drm_device *drm_dev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	void __iomem *mmio;
};

int dp_hw_init(struct hibmc_dp *dp);
void dp_hw_uninit(struct hibmc_dp *dp);
int dp_mode_set(struct hibmc_dp *dp, struct drm_display_mode *mode);
void dp_display_en(struct hibmc_dp *dp, bool enable);

#endif
