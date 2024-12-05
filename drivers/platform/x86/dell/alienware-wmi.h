// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Alienware AlienFX control
 *
 * Copyright (C) 2014 Dell Inc <Dell.Client.Kernel@dell.com>
 */

#ifndef _ALIENWARE_WMI_H_
#define _ALIENWARE_WMI_H_

#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/wmi.h>

#define LEGACY_CONTROL_GUID		"A90597CE-A997-11DA-B012-B622A1EF5492"
#define LEGACY_POWER_CONTROL_GUID	"A80593CE-A997-11DA-B012-B622A1EF5492"
#define WMAX_CONTROL_GUID		"A70591CE-A997-11DA-B012-B622A1EF5492"

#define WMAX_METHOD_HDMI_SOURCE		0x1
#define WMAX_METHOD_HDMI_STATUS		0x2
#define WMAX_METHOD_BRIGHTNESS		0x3
#define WMAX_METHOD_ZONE_CONTROL	0x4
#define WMAX_METHOD_HDMI_CABLE		0x5
#define WMAX_METHOD_AMPLIFIER_CABLE	0x6
#define WMAX_METHOD_DEEP_SLEEP_CONTROL	0x0B
#define WMAX_METHOD_DEEP_SLEEP_STATUS	0x0C

enum LEGACY_CONTROL_STATES {
	LEGACY_RUNNING = 1,
	LEGACY_BOOTING = 0,
	LEGACY_SUSPEND = 3,
};

enum WMAX_CONTROL_STATES {
	WMAX_RUNNING = 0xFF,
	WMAX_BOOTING = 0,
	WMAX_SUSPEND = 3,
};

enum wmax_thermal_mode {
	THERMAL_MODE_USTT_BALANCED,
	THERMAL_MODE_USTT_BALANCED_PERFORMANCE,
	THERMAL_MODE_USTT_COOL,
	THERMAL_MODE_USTT_QUIET,
	THERMAL_MODE_USTT_PERFORMANCE,
	THERMAL_MODE_USTT_LOW_POWER,
	THERMAL_MODE_BASIC_QUIET,
	THERMAL_MODE_BASIC_BALANCED,
	THERMAL_MODE_BASIC_BALANCED_PERFORMANCE,
	THERMAL_MODE_BASIC_PERFORMANCE,
	THERMAL_MODE_LAST,
};

struct color_platform {
	u8 blue;
	u8 green;
	u8 red;
} __packed;

struct legacy_led_args {
	struct color_platform colors;
	u8 brightness;
	u8 state;
} __packed;

struct wmax_led_args {
	u32 led_mask;
	struct color_platform colors;
	u8 state;
} __packed;

struct wmax_brightness_args {
	u32 led_mask;
	u32 percentage;
};

struct alienfx_priv {
	struct platform_device *pdev;
	struct led_classdev global_led;
	struct color_platform colors[4];
	u8 global_brightness;
	u8 lighting_control_state;
};

struct alienfx_ops {
	int (*upd_led)(struct alienfx_priv *priv, struct wmi_device *wdev,
		       u8 location);
	int (*upd_brightness)(struct alienfx_priv *priv, struct wmi_device *wdev,
			      u8 brightness);
};

struct alienfx_platdata {
	struct wmi_device *wdev;
	struct alienfx_ops ops;
	u8 num_zones;
	bool hdmi_mux;
	bool amplifier;
	bool deepslp;
	u8 running_code;
};

struct awcc_priv {
	struct wmi_device *wdev;
	bool has_gmode;
	struct platform_profile_handler pp_handler;
	enum wmax_thermal_mode supported_thermal_profiles[PLATFORM_PROFILE_LAST];
};

acpi_status alienware_wmi_command(struct wmi_device *wdev, u32 method_id,
				  void *in_args, size_t in_size, u32 *out_data);

#if IS_ENABLED(CONFIG_ALIENWARE_ALIENFX)
int alienfx_wmi_init(struct alienfx_platdata *pdata);
void alienfx_wmi_exit(struct wmi_device *wdev);
#else
int inline alienfx_wmi_init(struct alienfx_platdata *pdata)
{
	return 0;
}

void inline alienfx_wmi_exit(struct wmi_device *wdev)
{
}
#endif

#if IS_ENABLED(CONFIG_ALIENWARE_AWCC)
int create_thermal_profile(struct wmi_device *wdev, bool has_gmode);
void remove_thermal_profile(void);
#else
int inline create_thermal_profile(struct wmi_device *wdev, bool has_gmode)
{
	return 0;
}

void inline remove_thermal_profile(void)
{
}
#endif

#endif
