// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo GameZone WMI interface driver. The GameZone WMI interface provides
 * platform profile and fan curve settings for devices that fall under the
 * "Gaming Series" of Lenovo Legion devices.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/platform_profile.h>
#include "lenovo-wmi.h"

#define LENOVO_GAMEZONE_GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"

/* Method IDs */
#define WMI_METHOD_ID_SMARTFAN_SUPP 43 /* IsSupportSmartFan */
#define WMI_METHOD_ID_SMARTFAN_SET 44 /* SetSmartFanMode */
#define WMI_METHOD_ID_SMARTFAN_GET 45 /* GetSmartFanMode */

static DEFINE_MUTEX(call_mutex);

static const struct wmi_device_id lenovo_wmi_gamezone_id_table[] = {
	{ LENOVO_GAMEZONE_GUID, NULL }, /* LENOVO_GAMEZONE_DATA */
	{}
};

struct lenovo_wmi_gz_priv {
	struct wmi_device *wdev;
	enum platform_profile_option current_profile;
	struct platform_profile_handler pprof;
	bool platform_profile_support;
};

/* Platform Profile Methods */
static int lenovo_wmi_gamezone_platform_profile_supported(
	struct platform_profile_handler *pprof, int *supported)
{
	struct lenovo_wmi_gz_priv *priv;

	priv = container_of(pprof, struct lenovo_wmi_gz_priv, pprof);

	guard(mutex)(&call_mutex);
	return lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SUPP, 0, supported);
}

static int
lenovo_wmi_gamezone_profile_get(struct platform_profile_handler *pprof,
				enum platform_profile_option *profile)
{
	struct lenovo_wmi_gz_priv *priv;
	int sel_prof;
	int err;

	priv = container_of(pprof, struct lenovo_wmi_gz_priv, pprof);

	guard(mutex)(&call_mutex);
	err = lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_GET, 0, &sel_prof);
	if (err) {
		pr_err("Error getting fan profile from WMI interface: %d\n",
		       err);
		return err;
	}

	switch (sel_prof) {
	case SMARTFAN_MODE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case SMARTFAN_MODE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SMARTFAN_MODE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_CUSTOM:
		*profile = PLATFORM_PROFILE_CUSTOM;
		break;
	default:
		return -EINVAL;
	}
	priv->current_profile = *profile;

	return 0;
}

static int
lenovo_wmi_gamezone_profile_set(struct platform_profile_handler *pprof,
				enum platform_profile_option profile)
{
	struct lenovo_wmi_gz_priv *priv;
	int sel_prof;
	int err;

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		sel_prof = SMARTFAN_MODE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		sel_prof = SMARTFAN_MODE_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_CUSTOM:
		sel_prof = SMARTFAN_MODE_CUSTOM;
		break;
	default:
		return -EOPNOTSUPP;
	}

	priv = container_of(pprof, struct lenovo_wmi_gz_priv, pprof);

	guard(mutex)(&call_mutex);
	err = lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SET, sel_prof, NULL);
	if (err) {
		pr_err("Error setting fan profile on WMI interface: %u\n", err);
		return err;
	}

	priv->current_profile = profile;
	return 0;
}

/* Driver Setup */
static int platform_profile_setup(struct lenovo_wmi_gz_priv *priv)
{
	int supported;
	int err;

	err = lenovo_wmi_gamezone_platform_profile_supported(&priv->pprof,
							     &supported);
	if (err) {
		pr_err("Error checking platform profile support: %d\n", err);
		return err;
	}

	priv->platform_profile_support = supported;

	if (!supported)
		return -EOPNOTSUPP;

	priv->pprof.name = "lenovo-wmi-gamezone";
	priv->pprof.profile_get = lenovo_wmi_gamezone_profile_get;
	priv->pprof.profile_set = lenovo_wmi_gamezone_profile_set;

	set_bit(PLATFORM_PROFILE_QUIET, priv->pprof.choices);
	set_bit(PLATFORM_PROFILE_BALANCED, priv->pprof.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, priv->pprof.choices);
	set_bit(PLATFORM_PROFILE_CUSTOM, priv->pprof.choices);

	err = lenovo_wmi_gamezone_profile_get(&priv->pprof,
					      &priv->current_profile);
	if (err) {
		pr_err("Error getting current platform profile: %d\n", err);
		return err;
	}

	guard(mutex)(&call_mutex);
	err = platform_profile_register(&priv->pprof);
	if (err) {
		pr_err("Error registering platform profile: %d\n", err);
		return err;
	}

	return 0;
}

static int lenovo_wmi_gamezone_probe(struct wmi_device *wdev,
				     const void *context)
{
	struct lenovo_wmi_gz_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	return platform_profile_setup(priv);
}

static void lenovo_wmi_gamezone_remove(struct wmi_device *wdev)
{
	struct lenovo_wmi_gz_priv *priv = dev_get_drvdata(&wdev->dev);

	guard(mutex)(&call_mutex);
	platform_profile_remove(&priv->pprof);
}

static struct wmi_driver lenovo_wmi_gamezone_driver = {
	.driver = { .name = "lenovo_wmi_gamezone" },
	.id_table = lenovo_wmi_gamezone_id_table,
	.probe = lenovo_wmi_gamezone_probe,
	.remove = lenovo_wmi_gamezone_remove,
};

module_wmi_driver(lenovo_wmi_gamezone_driver);

MODULE_DEVICE_TABLE(wmi, lenovo_wmi_gamezone_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo GameZone WMI Driver");
MODULE_LICENSE("GPL");
