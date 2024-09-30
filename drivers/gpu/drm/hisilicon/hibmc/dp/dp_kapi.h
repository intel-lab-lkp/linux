/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_KAPI_H
#define DP_KAPI_H

#include <linux/types.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_print.h>
#include <linux/delay.h>

struct hibmc_dp_dev;

struct dp_mode {
	u32 h_total;
	u32 h_active;
	u32 h_blank;
	u32 h_front;
	u32 h_sync;
	u32 h_back;
	bool h_pol;
	u32 v_total;
	u32 v_active;
	u32 v_blank;
	u32 v_front;
	u32 v_sync;
	u32 v_back;
	bool v_pol;
	u32 field_rate;
	u32 pixel_clock; // khz
};

struct hibmc_dp {
	struct hibmc_dp_dev *dp_dev;
	struct drm_device *drm_dev;
	struct drm_encoder encoder;
	struct drm_connector connector;
	void __iomem *mmio;
};

int hibmc_dp_kapi_init(struct hibmc_dp *dp);
void hibmc_dp_kapi_uninit(struct hibmc_dp *dp);
int hibmc_dp_mode_set(struct hibmc_dp *dp, struct dp_mode *mode);
void hibmc_dp_display_en(struct hibmc_dp *dp, bool enable);

#endif
