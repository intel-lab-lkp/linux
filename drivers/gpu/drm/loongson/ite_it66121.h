/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __ITE_IT66121_H__
#define __ITE_IT66121_H__

#include <drm/drm_bridge.h>
#include <drm/drm_device.h>

struct drm_bridge *it66121_bridge_create(struct drm_device *ddev,
					 struct i2c_adapter *i2c,
					 u8 addr,
					 bool enable_hpd,
					 u32 int_gpio,
					 unsigned int pipe);

#endif
