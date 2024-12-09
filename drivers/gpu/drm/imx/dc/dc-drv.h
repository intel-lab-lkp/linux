/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 NXP
 */

#ifndef __DC_DRV_H__
#define __DC_DRV_H__

#include <linux/platform_device.h>

#include <drm/drm_device.h>

#include "dc-de.h"

struct dc_drm_device {
	struct drm_device base;
	struct dc_de *de[DC_DISPLAYS];
};

extern struct platform_driver dc_de_driver;
extern struct platform_driver dc_fg_driver;
extern struct platform_driver dc_tc_driver;

#endif /* __DC_DRV_H__ */
