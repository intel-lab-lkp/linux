/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#ifndef __LOONGSON_VBIOS_H__
#define __LOONGSON_VBIOS_H__

#include <drm/drm_device.h>

struct loongson_vbios {
	struct list_head list;
	void *raw_data;
	struct drm_device *ddev;
	u32 version_major;
	u32 version_minor;
};

enum loongson_vbios_encoder_name {
	ENCODER_CHIP_UNKNOWN = 0x00,
	ENCODER_CHIP_INTERNAL_VGA = 0x01,
	ENCODER_CHIP_INTERNAL_HDMI = 0x02,
	ENCODER_CHIP_CH7055 = 0x10,
	ENCODER_CHIP_ADV7125 = 0x11,
	ENCODER_CHIP_TFP410 = 0x20,
	ENCODER_CHIP_IT66121 = 0x30,
	ENCODER_CHIP_SIL9022 = 0x31,
	ENCODER_CHIP_LT8618 = 0x32,
	ENCODER_CHIP_MS7210 = 0x33,
	ENCODER_CHIP_NCS8805 = 0x40,
	ENCODER_CHIP_LT9721 = 0x42,
	ENCODER_CHIP_LT6711 = 0x43,
	ENCODER_CHIP_LT8619 = 0x50,
};

enum loongson_vbios_hotplug_method {
	LV_HPD_DISABLED = 0,
	LV_HPD_POLLING = 1,
	LV_HPD_IRQ = 2,
};

const struct loongson_vbios *to_loongson_vbios(struct drm_device *ddev);

bool loongson_vbios_query_encoder_info(struct drm_device *ddev,
				       u32 pipe,
				       u32 *type,
				       enum loongson_vbios_encoder_name *name,
				       u8 *i2c_addr);

bool loongson_vbios_query_connector_info(struct drm_device *ddev,
					 u32 pipe,
					 u32 *connector_type,
					 u32 *hpd_method,
					 u32 *int_gpio,
					 u8 *edid_blob);

int loongson_vbios_init(struct drm_device *ddev);

#endif
