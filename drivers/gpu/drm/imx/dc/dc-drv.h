/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 NXP
 */

#ifndef __DC_DRV_H__
#define __DC_DRV_H__

#include <linux/container_of.h>
#include <linux/platform_device.h>

#include <drm/drm_device.h>
#include <drm/drm_encoder.h>

#include "dc-de.h"
#include "dc-kms.h"
#include "dc-pe.h"

struct dc_drm_device {
	struct drm_device base;
	struct dc_crtc dc_crtc[DC_DISPLAYS];
	struct dc_plane dc_primary[DC_DISPLAYS];
	struct drm_encoder encoder[DC_DISPLAYS];
	struct dc_de *de[DC_DISPLAYS];
	struct dc_pe *pe;
};

static inline struct dc_drm_device *to_dc_drm_device(struct drm_device *drm)
{
	return container_of(drm, struct dc_drm_device, base);
}

int dc_crtc_init(struct dc_drm_device *dc_drm, int crtc_index);
int dc_crtc_post_init(struct dc_drm_device *dc_drm, int crtc_index);

int dc_kms_init(struct dc_drm_device *dc_drm);
void dc_kms_uninit(struct dc_drm_device *dc_drm);

int dc_plane_init(struct dc_drm_device *dc_drm, struct dc_plane *dc_plane);

extern struct platform_driver dc_cf_driver;
extern struct platform_driver dc_ed_driver;
extern struct platform_driver dc_de_driver;
extern struct platform_driver dc_fg_driver;
extern struct platform_driver dc_fl_driver;
extern struct platform_driver dc_fw_driver;
extern struct platform_driver dc_ic_driver;
extern struct platform_driver dc_lb_driver;
extern struct platform_driver dc_pe_driver;
extern struct platform_driver dc_tc_driver;

#endif /* __DC_DRV_H__ */
