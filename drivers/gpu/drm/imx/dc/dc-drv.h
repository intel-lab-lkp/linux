/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 NXP
 */

#ifndef __DC_DRV_H__
#define __DC_DRV_H__

#include <linux/platform_device.h>

#include <drm/drm_device.h>

#include "dc-de.h"
#include "dc-pe.h"

struct dc_drm_device {
	struct drm_device base;
	struct dc_de *de[DC_DISPLAYS];
	struct dc_pe *pe;
};

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
