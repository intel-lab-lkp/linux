// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Alienware AlienFX control
 *
 * Copyright (C) 2024 Kurt Borja <kuurtb@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/wmi.h>
#include "alienware-wmi.h"

#define WMAX_METHOD_THERMAL_INFORMATION	0x14
#define WMAX_METHOD_THERMAL_CONTROL	0x15
#define WMAX_METHOD_GAME_SHIFT_STATUS	0x25

#define WMAX_THERMAL_MODE_GMODE		0xAB

#define WMAX_FAILURE_CODE		0xFFFFFFFF
#define WMAX_THERMAL_TABLE_MASK		GENMASK(7, 4)
#define WMAX_THERMAL_MODE_MASK		GENMASK(3, 0)
#define WMAX_SENSOR_ID_MASK		BIT(8)

enum WMAX_THERMAL_INFORMATION_OPERATIONS {
	WMAX_OPERATION_SYS_DESCRIPTION		= 0x02,
	WMAX_OPERATION_LIST_IDS			= 0x03,
	WMAX_OPERATION_CURRENT_PROFILE		= 0x0B,
};

enum WMAX_THERMAL_CONTROL_OPERATIONS {
	WMAX_OPERATION_ACTIVATE_PROFILE		= 0x01,
};

enum WMAX_GAME_SHIFT_STATUS_OPERATIONS {
	WMAX_OPERATION_TOGGLE_GAME_SHIFT	= 0x01,
	WMAX_OPERATION_GET_GAME_SHIFT_STATUS	= 0x02,
};

enum WMAX_THERMAL_TABLES {
	WMAX_THERMAL_TABLE_BASIC		= 0x90,
	WMAX_THERMAL_TABLE_USTT			= 0xA0,
};

static const enum platform_profile_option wmax_mode_to_platform_profile[THERMAL_MODE_LAST] = {
	[THERMAL_MODE_USTT_BALANCED]			= PLATFORM_PROFILE_BALANCED,
	[THERMAL_MODE_USTT_BALANCED_PERFORMANCE]	= PLATFORM_PROFILE_BALANCED_PERFORMANCE,
	[THERMAL_MODE_USTT_COOL]			= PLATFORM_PROFILE_COOL,
	[THERMAL_MODE_USTT_QUIET]			= PLATFORM_PROFILE_QUIET,
	[THERMAL_MODE_USTT_PERFORMANCE]			= PLATFORM_PROFILE_PERFORMANCE,
	[THERMAL_MODE_USTT_LOW_POWER]			= PLATFORM_PROFILE_LOW_POWER,
	[THERMAL_MODE_BASIC_QUIET]			= PLATFORM_PROFILE_QUIET,
	[THERMAL_MODE_BASIC_BALANCED]			= PLATFORM_PROFILE_BALANCED,
	[THERMAL_MODE_BASIC_BALANCED_PERFORMANCE]	= PLATFORM_PROFILE_BALANCED_PERFORMANCE,
	[THERMAL_MODE_BASIC_PERFORMANCE]		= PLATFORM_PROFILE_PERFORMANCE,
};

struct wmax_u32_args {
	u8 operation;
	u8 arg1;
	u8 arg2;
	u8 arg3;
};

/*
 * Thermal Profile control
 *  - Provides thermal profile control through the Platform Profile API
 */
static bool is_wmax_thermal_code(u32 code)
{
	if (code & WMAX_SENSOR_ID_MASK)
		return false;

	if ((code & WMAX_THERMAL_MODE_MASK) >= THERMAL_MODE_LAST)
		return false;

	if ((code & WMAX_THERMAL_TABLE_MASK) == WMAX_THERMAL_TABLE_BASIC &&
	    (code & WMAX_THERMAL_MODE_MASK) >= THERMAL_MODE_BASIC_QUIET)
		return true;

	if ((code & WMAX_THERMAL_TABLE_MASK) == WMAX_THERMAL_TABLE_USTT &&
	    (code & WMAX_THERMAL_MODE_MASK) <= THERMAL_MODE_USTT_LOW_POWER)
		return true;

	return false;
}

static int wmax_thermal_information(struct wmi_device *wdev, u8 operation,
				    u8 arg, u32 *out_data)
{
	acpi_status status;
	struct wmax_u32_args in_args = {
		.operation = operation,
		.arg1 = arg,
		.arg2 = 0,
		.arg3 = 0,
	};

	status = alienware_wmi_command(wdev, WMAX_METHOD_THERMAL_INFORMATION,
				       &in_args, sizeof(in_args), out_data);

	if (ACPI_FAILURE(status))
		return -EIO;

	if (*out_data == WMAX_FAILURE_CODE)
		return -EBADRQC;

	return 0;
}

static int wmax_thermal_control(struct wmi_device *wdev, u8 profile)
{
	acpi_status status;
	struct wmax_u32_args in_args = {
		.operation = WMAX_OPERATION_ACTIVATE_PROFILE,
		.arg1 = profile,
		.arg2 = 0,
		.arg3 = 0,
	};
	u32 out_data;

	status = alienware_wmi_command(wdev, WMAX_METHOD_THERMAL_CONTROL,
				       &in_args, sizeof(in_args), &out_data);

	if (ACPI_FAILURE(status))
		return -EIO;

	if (out_data == WMAX_FAILURE_CODE)
		return -EBADRQC;

	return 0;
}

static int wmax_game_shift_status(struct wmi_device *wdev, u8 operation,
				  u32 *out_data)
{
	acpi_status status;
	struct wmax_u32_args in_args = {
		.operation = operation,
		.arg1 = 0,
		.arg2 = 0,
		.arg3 = 0,
	};

	status = alienware_wmi_command(wdev, WMAX_METHOD_GAME_SHIFT_STATUS,
				       &in_args, sizeof(in_args), out_data);

	if (ACPI_FAILURE(status))
		return -EIO;

	if (*out_data == WMAX_FAILURE_CODE)
		return -EOPNOTSUPP;

	return 0;
}

static int thermal_profile_get(struct platform_profile_handler *pprof,
			       enum platform_profile_option *profile)
{
	struct awcc_priv *priv;
	u32 out_data;
	int ret;

	priv = container_of(pprof, struct awcc_priv, pp_handler);

	ret = wmax_thermal_information(priv->wdev, WMAX_OPERATION_CURRENT_PROFILE,
				       0, &out_data);

	if (ret < 0)
		return ret;

	if (out_data == WMAX_THERMAL_MODE_GMODE) {
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		return 0;
	}

	if (!is_wmax_thermal_code(out_data))
		return -ENODATA;

	out_data &= WMAX_THERMAL_MODE_MASK;
	*profile = wmax_mode_to_platform_profile[out_data];

	return 0;
}

static int thermal_profile_set(struct platform_profile_handler *pprof,
			       enum platform_profile_option profile)
{
	struct awcc_priv *priv;

	priv = container_of(pprof, struct awcc_priv, pp_handler);

	if (priv->has_gmode) {
		u32 gmode_status;
		int ret;

		ret = wmax_game_shift_status(priv->wdev,
					     WMAX_OPERATION_GET_GAME_SHIFT_STATUS,
					     &gmode_status);

		if (ret < 0)
			return ret;

		if ((profile == PLATFORM_PROFILE_PERFORMANCE && !gmode_status) ||
		    (profile != PLATFORM_PROFILE_PERFORMANCE && gmode_status)) {
			ret = wmax_game_shift_status(priv->wdev,
						     WMAX_OPERATION_TOGGLE_GAME_SHIFT,
						     &gmode_status);

			if (ret < 0)
				return ret;
		}
	}

	return wmax_thermal_control(priv->wdev,
				    priv->supported_thermal_profiles[profile]);
}

int create_thermal_profile(struct wmi_device *wdev, bool has_gmode)
{
	struct awcc_priv *priv;
	u32 out_data;
	u8 sys_desc[4];
	u32 first_mode;
	enum wmax_thermal_mode mode;
	enum platform_profile_option profile;
	int ret;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	dev_set_drvdata(&wdev->dev, priv);

	priv->wdev = wdev;

	ret = wmax_thermal_information(wdev, WMAX_OPERATION_SYS_DESCRIPTION,
				       0, (u32 *) &sys_desc);
	if (ret < 0)
		return ret;

	first_mode = sys_desc[0] + sys_desc[1];

	for (u32 i = 0; i < sys_desc[3]; i++) {
		ret = wmax_thermal_information(wdev, WMAX_OPERATION_LIST_IDS,
					       i + first_mode, &out_data);

		if (ret == -EIO)
			return ret;

		if (ret == -EBADRQC)
			break;

		if (!is_wmax_thermal_code(out_data))
			continue;

		mode = out_data & WMAX_THERMAL_MODE_MASK;
		profile = wmax_mode_to_platform_profile[mode];
		priv->supported_thermal_profiles[profile] = out_data;

		set_bit(profile, priv->pp_handler.choices);
	}

	if (bitmap_empty(priv->pp_handler.choices, PLATFORM_PROFILE_LAST))
		return -ENODEV;

	if (has_gmode) {
		priv->has_gmode = true;
		priv->supported_thermal_profiles[PLATFORM_PROFILE_PERFORMANCE] =
			WMAX_THERMAL_MODE_GMODE;

		set_bit(PLATFORM_PROFILE_PERFORMANCE, priv->pp_handler.choices);
	}

	priv->pp_handler.profile_get = thermal_profile_get;
	priv->pp_handler.profile_set = thermal_profile_set;

	return platform_profile_register(&priv->pp_handler);
}

void remove_thermal_profile(void)
{
	platform_profile_remove();
}
