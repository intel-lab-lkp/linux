// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  WMI driver for Dell's AWCC platform_profile
 *
 *  Copyright (c) Kurt Borja <kuurtb@gmail.com>
 *
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_profile.h>
#include <linux/wmi.h>

#define PROF_TO_ARG(mode) ((mode << 8) | 1)

#define DELL_AWCC_GUID "A70591CE-A997-11DA-B012-B622A1EF5492"

enum awcc_wmi_method {
	AWCC_WMI_THERMAL_INFORMATION = 0x14,
	AWCC_WMI_THERMAL_CONTROL = 0x15,
};

enum awcc_tmp_profile {
	AWCC_TMP_PROFILE_BALANCED = 0xA0,
	AWCC_TMP_PROFILE_BALANCED_PERFORMANCE = 0xA1,
	AWCC_TMP_PROFILE_COOL = 0xA2,
	AWCC_TMP_PROFILE_QUIET = 0xA3,
	AWCC_TMP_PROFILE_PERFORMANCE = 0xA4,
	AWCC_TMP_PROFILE_LOW_POWER = 0xA5,
};

struct awcc_wmi_priv {
	struct wmi_device *wdev;
	struct platform_profile_handler handler;
};

static int awcc_wmi_query(struct wmi_device *wdev, enum awcc_wmi_method method,
			  u32 arg, u32 *res)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	const struct acpi_buffer in = { sizeof(arg), &arg };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	status = wmidev_evaluate_method(wdev, 0x0, method, &in, &out);

	if (ACPI_FAILURE(status))
		return -EIO;

	obj = out.pointer;
	if (!obj)
		return -ENODATA;

	if (obj->type != ACPI_TYPE_INTEGER) {
		ret = -EINVAL;
		goto out_free;
	}

	if (obj->integer.value <= U32_MAX)
		*res = (u32)obj->integer.value;
	else
		ret = -ERANGE;

out_free:
	kfree(obj);

	return ret;
}

static int awcc_platform_profile_get(struct platform_profile_handler *pprof,
				     enum platform_profile_option *profile)
{
	struct awcc_wmi_priv *priv =
		container_of(pprof, struct awcc_wmi_priv, handler);

	u32 res;
	int ret;

	ret = awcc_wmi_query(priv->wdev, AWCC_WMI_THERMAL_INFORMATION, 0x0B,
			     &res);

	if (ret < 0)
		return ret;

	if (res < 0)
		return -EBADRQC;

	switch (res) {
	case AWCC_TMP_PROFILE_LOW_POWER:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case AWCC_TMP_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case AWCC_TMP_PROFILE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case AWCC_TMP_PROFILE_BALANCED_PERFORMANCE:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case AWCC_TMP_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	default:
		return -ENODATA;
	}

	return 0;
}

static int awcc_platform_profile_set(struct platform_profile_handler *pprof,
				     enum platform_profile_option profile)
{
	struct awcc_wmi_priv *priv =
		container_of(pprof, struct awcc_wmi_priv, handler);

	u32 arg;
	u32 res;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		arg = PROF_TO_ARG(AWCC_TMP_PROFILE_LOW_POWER);
		break;
	case PLATFORM_PROFILE_QUIET:
		arg = PROF_TO_ARG(AWCC_TMP_PROFILE_QUIET);
		break;
	case PLATFORM_PROFILE_BALANCED:
		arg = PROF_TO_ARG(AWCC_TMP_PROFILE_BALANCED);
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		arg = PROF_TO_ARG(AWCC_TMP_PROFILE_BALANCED_PERFORMANCE);
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		arg = PROF_TO_ARG(AWCC_TMP_PROFILE_PERFORMANCE);
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = awcc_wmi_query(priv->wdev, AWCC_WMI_THERMAL_CONTROL, arg, &res);

	if (ret < 0)
		return ret;

	if (res < 0)
		return -EBADRQC;

	return 0;
}

static int awcc_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct awcc_wmi_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	priv->handler.profile_set = awcc_platform_profile_set;
	priv->handler.profile_get = awcc_platform_profile_get;

	set_bit(PLATFORM_PROFILE_LOW_POWER, priv->handler.choices);
	set_bit(PLATFORM_PROFILE_QUIET, priv->handler.choices);
	set_bit(PLATFORM_PROFILE_BALANCED, priv->handler.choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, priv->handler.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, priv->handler.choices);

	return platform_profile_register(&priv->handler);
}

static void awcc_wmi_remove(struct wmi_device *wdev)
{
	platform_profile_remove();
}

static const struct wmi_device_id awcc_wmi_id_table[] = {
	{ .guid_string = DELL_AWCC_GUID },
	{},
};

MODULE_DEVICE_TABLE(wmi, awcc_wmi_id_table);

static struct wmi_driver awcc_wmi_driver = {
	.driver = {
		.name = "dell-wmi-awcc-platform-profile",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = awcc_wmi_id_table,
	.probe = awcc_wmi_probe,
	.remove = awcc_wmi_remove,
	.no_singleton = true,
};

module_wmi_driver(awcc_wmi_driver);

MODULE_AUTHOR("Kurt Borja");
MODULE_DESCRIPTION("Dell AWCC WMI driver");
MODULE_LICENSE("GPL");
