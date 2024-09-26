/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This code gives functions to avoid code duplication while interacting with
 * the TUXEDO NB04 wmi interfaces.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#ifndef TUXEDO_NB04_WMI_UTIL_H
#define TUXEDO_NB04_WMI_UTIL_H

#include <linux/wmi.h>

#define WMI_AB_GET_DEVICE_STATUS_DEVICE_ID_TOUCHPAD	1
#define WMI_AB_GET_DEVICE_STATUS_DEVICE_ID_KEYBOARD	2
#define WMI_AB_GET_DEVICE_STATUS_DEVICE_ID_APP_PAGES	3

#define WMI_AB_GET_DEVICE_STATUS_KBL_TYPE_NONE		0
#define WMI_AB_GET_DEVICE_STATUS_KBL_TYPE_PER_KEY	1
#define WMI_AB_GET_DEVICE_STATUS_KBL_TYPE_FOUR_ZONE	2
#define WMI_AB_GET_DEVICE_STATUS_KBL_TYPE_WHITE_ONLY	3

#define WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ANSII	0
#define WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ISO	1

#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_RED		1
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_GREEN		2
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_YELLOW	3
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_BLUE		4
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_PURPLE	5
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_INDIGO	6
#define WMI_AB_GET_DEVICE_STATUS_COLOR_ID_WHITE		7

#define WMI_AB_GET_DEVICE_STATUS_APP_PAGES_DASHBOARD	BIT(0)
#define WMI_AB_GET_DEVICE_STATUS_APP_PAGES_SYSTEMINFOS	BIT(1)
#define WMI_AB_GET_DEVICE_STATUS_APP_PAGES_KBL		BIT(2)
#define WMI_AB_GET_DEVICE_STATUS_APP_PAGES_HOTKEYS	BIT(3)


union tuxedo_nb04_wmi_8_b_in_80_b_out_input {
	uint8_t raw[8];
	struct __packed {
		uint8_t device_type;
		uint8_t reserved_0[7];
	} get_device_status_input;
};

union tuxedo_nb04_wmi_8_b_in_80_b_out_output {
	uint8_t raw[80];
	struct __packed {
		uint16_t return_status;
		uint8_t device_enabled;
		uint8_t kbl_type;
		uint8_t kbl_side_bar_supported;
		uint8_t keyboard_physical_layout;
		uint8_t app_pages;
		uint8_t per_key_kbl_default_color;
		uint8_t four_zone_kbl_default_color_1;
		uint8_t four_zone_kbl_default_color_2;
		uint8_t four_zone_kbl_default_color_3;
		uint8_t four_zone_kbl_default_color_4;
		uint8_t light_bar_kbl_default_color;
		uint8_t reserved_0[1];
		uint16_t dedicated_gpu_id;
		uint8_t reserved_1[64];
	} get_device_status_output;
};

enum tuxedo_nb04_wmi_8_b_in_80_b_out_methods {
	WMI_AB_GET_DEVICE_STATUS	= 2,
};


#define WMI_AB_KBL_SET_MULTIPLE_KEYS_LIGHTING_SETTINGS_COUNT_MAX	120

union tuxedo_nb04_wmi_496_b_in_80_b_out_input {
	uint8_t raw[496];
	struct __packed {
		uint8_t reserved_0[15];
		uint8_t lighting_setting_count;
		struct {
			uint8_t key_id;
			uint8_t red;
			uint8_t green;
			uint8_t blue;
		} lighting_settings[WMI_AB_KBL_SET_MULTIPLE_KEYS_LIGHTING_SETTINGS_COUNT_MAX];
	}  kbl_set_multiple_keys_input;
};

union tuxedo_nb04_wmi_496_b_in_80_b_out_output {
	uint8_t raw[80];
	struct __packed {
		uint8_t return_value;
		uint8_t reserved_0[79];
	} kbl_set_multiple_keys_output;
};

enum tuxedo_nb04_wmi_496_b_in_80_b_out_methods {
	WMI_AB_KBL_SET_MULTIPLE_KEYS	= 6,
};


int tuxedo_nb04_wmi_8_b_in_80_b_out(struct wmi_device *wdev,
				    enum tuxedo_nb04_wmi_8_b_in_80_b_out_methods method,
				    union tuxedo_nb04_wmi_8_b_in_80_b_out_input *input,
				    union tuxedo_nb04_wmi_8_b_in_80_b_out_output *output);
int tuxedo_nb04_wmi_496_b_in_80_b_out(struct wmi_device *wdev,
				      enum tuxedo_nb04_wmi_496_b_in_80_b_out_methods method,
				      union tuxedo_nb04_wmi_496_b_in_80_b_out_input *input,
				      union tuxedo_nb04_wmi_496_b_in_80_b_out_output *output);

#endif
