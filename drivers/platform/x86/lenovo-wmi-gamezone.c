// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo GameZone WMI interface driver. The GameZone WMI interface provides
 * platform profile and fan curve settings for devices that fall under the
 * "Gaming Series" of Lenovo Legion devices.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include "linux/container_of.h"
#include "linux/printk.h"
#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/dmi.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/platform_profile.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include "lenovo-wmi.h"

/* Interface GUIDs */
#define LENOVO_GAMEZONE_GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"
#define THERMAL_MODE_EVENT_GUID "D320289E-8FEA-41E0-86F9-911D83151B5F"

/* Method IDs */
#define WMI_METHOD_ID_SMARTFAN_SUPP 43 /* IsSupportSmartFan */
#define WMI_METHOD_ID_SMARTFAN_SET 44 /* SetSmartFanMode */
#define WMI_METHOD_ID_SMARTFAN_GET 45 /* GetSmartFanMode */

enum lenovo_wmi_gz_type {
	GAMEZONE_FULL = 1,
	THERMAL_MODE,
};

#define GAMEZONE_WMI_DEVICE(guid, type)                              \
	.guid_string = (guid), .context = &(enum lenovo_wmi_gz_type) \
	{                                                            \
		type                                                 \
	}

static BLOCKING_NOTIFIER_HEAD(gz_chain_head);
static DEFINE_MUTEX(gz_chain_mutex);

struct lenovo_wmi_gz_priv {
	enum platform_profile_option current_profile;
	struct wmi_device *wdev;
	bool extreme_supported;
	struct device *ppdev; /*platform profile device */
	enum lenovo_wmi_gz_type type;
	struct blocking_notifier_head nhead;
};

struct quirk_entry {
	bool extreme_supported;
};

static struct quirk_entry quirk_no_extreme_bug = {
	.extreme_supported = false,
};

/* Platform Profile Methods & Setup */
static int
lenovo_wmi_gz_platform_profile_supported(struct lenovo_wmi_gz_priv *priv,
					 int *supported)
{
	return lenovo_wmidev_evaluate_method_1(priv->wdev, 0x0,
					       WMI_METHOD_ID_SMARTFAN_SUPP, 0, supported);
}

static int lenovo_wmi_gz_profile_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	struct lenovo_wmi_gz_priv *priv = dev_get_drvdata(dev);
	int sel_prof;
	int ret;

	ret = lenovo_wmidev_evaluate_method_1(priv->wdev, 0x0,
					      WMI_METHOD_ID_SMARTFAN_GET, 0, &sel_prof);
	if (ret)
		return ret;

	switch (sel_prof) {
	case SMARTFAN_MODE_QUIET:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case SMARTFAN_MODE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SMARTFAN_MODE_PERFORMANCE:
		if (priv->extreme_supported) {
			*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
			break;
		}
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_EXTREME:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_CUSTOM:
		*profile = PLATFORM_PROFILE_CUSTOM;
		break;
	default:
		return -EINVAL;
	}

	priv->current_profile = *profile;

	ret = blocking_notifier_call_chain(&gz_chain_head, THERMAL_MODE_EVENT,
					   &sel_prof);
	if (ret == NOTIFY_BAD)
		pr_err("Failed to send notification to call chain for WMI event %u\n",
		       priv->type);
	return 0;
}

static int lenovo_wmi_gz_profile_set(struct device *dev,
				     enum platform_profile_option profile)
{
	struct lenovo_wmi_gz_priv *priv = dev_get_drvdata(dev);
	int sel_prof;
	int ret;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		sel_prof = SMARTFAN_MODE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		sel_prof = SMARTFAN_MODE_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		if (priv->extreme_supported) {
			sel_prof = SMARTFAN_MODE_EXTREME;
			break;
		}
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_CUSTOM:
		sel_prof = SMARTFAN_MODE_CUSTOM;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = lenovo_wmidev_evaluate_method_1(priv->wdev, 0x0,
					      WMI_METHOD_ID_SMARTFAN_SET, sel_prof, NULL);
	if (ret)
		return ret;

	return 0;
}

static const struct dmi_system_id fwbug_list[] = {
	{
		.ident = "Legion Go 8APU1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1"),
		},
		.driver_data = &quirk_no_extreme_bug,
	},
	{
		.ident = "Legion Go S 8ARP1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8ARP1"),
		},
		.driver_data = &quirk_no_extreme_bug,
	},
	{
		.ident = "Legion Go S 8APU1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8APU1"),
		},
		.driver_data = &quirk_no_extreme_bug,
	},
	{},

};

static bool extreme_supported(int profile_support_ver)
{
	const struct dmi_system_id *dmi_id;
	struct quirk_entry *quirks;

	if (profile_support_ver < 6)
		return false;

	dmi_id = dmi_first_match(fwbug_list);
	if (!dmi_id)
		return true;

	quirks = dmi_id->driver_data;
	return quirks->extreme_supported;
}

static int lenovo_wmi_platform_profile_probe(void *drvdata,
					     unsigned long *choices)
{
	struct lenovo_wmi_gz_priv *priv = drvdata;
	enum platform_profile_option profile;
	int profile_support_ver;
	int ret;

	ret = lenovo_wmi_gz_platform_profile_supported(priv,
						       &profile_support_ver);
	if (ret)
		return ret;

	if (profile_support_ver < 1)
		return -ENODEV;

	priv->extreme_supported = extreme_supported(profile_support_ver);

	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_CUSTOM, choices);

	if (priv->extreme_supported)
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);

	return 0;
}

static const struct platform_profile_ops lenovo_wmi_gz_platform_profile_ops = {
	.probe = lenovo_wmi_platform_profile_probe,
	.profile_get = lenovo_wmi_gz_profile_get,
	.profile_set = lenovo_wmi_gz_profile_set,
};

/* Notifier Methods */
int lenovo_wmi_gz_register_notifier(struct notifier_block *nb)
{
	guard(mutex)(&gz_chain_mutex);
	return blocking_notifier_chain_register(&gz_chain_head, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_gz_register_notifier, "GZ_WMI");

int lenovo_wmi_gz_unregister_notifier(struct notifier_block *nb)
{
	guard(mutex)(&gz_chain_mutex);
	return blocking_notifier_chain_unregister(&gz_chain_head, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_gz_unregister_notifier, "GZ_WMI");

static void devm_lenovo_wmi_gz_unregister_notifier(void *data)
{
	struct notifier_block *nb = data;

	lenovo_wmi_gz_unregister_notifier(nb);
}

int devm_lenovo_wmi_gz_register_notifier(struct device *dev,
					 struct notifier_block *nb)
{
	int ret;

	ret = lenovo_wmi_gz_register_notifier(nb);
	if (ret < 0)
		return ret;

	return devm_add_action_or_reset(dev, devm_lenovo_wmi_gz_unregister_notifier, nb);
}
EXPORT_SYMBOL_NS_GPL(devm_lenovo_wmi_gz_register_notifier, "GZ_WMI");

/* Driver Methods */
static void lenovo_wmi_gz_notify(struct wmi_device *wdev,
				 union acpi_object *obj)
{
	struct lenovo_wmi_gz_priv *tm_priv = dev_get_drvdata(&wdev->dev);
	struct lenovo_wmi_gz_priv *gz_priv =
		container_of(&gz_chain_head, struct lenovo_wmi_gz_priv, nhead);
	int sel_prof;
	int ret;

	if (obj->type != ACPI_TYPE_INTEGER)
		return;

	switch (tm_priv->type) {
	case THERMAL_MODE:
		sel_prof = obj->integer.value;
		break;
	default:
		return;
	}

	/* Update primary Gamezone instance */
	switch (sel_prof) {
	case SMARTFAN_MODE_QUIET:
		gz_priv->current_profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case SMARTFAN_MODE_BALANCED:
		gz_priv->current_profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SMARTFAN_MODE_PERFORMANCE:
		if (gz_priv->extreme_supported) {
			gz_priv->current_profile =
				PLATFORM_PROFILE_BALANCED_PERFORMANCE;
			break;
		}
		gz_priv->current_profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_EXTREME:
		gz_priv->current_profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_CUSTOM:
		gz_priv->current_profile = PLATFORM_PROFILE_CUSTOM;
		break;
	default:
		break;
	}

	ret = blocking_notifier_call_chain(&gz_chain_head, THERMAL_MODE_EVENT,
					   &sel_prof);
	if (ret == NOTIFY_BAD)
		pr_err("Failed to send notification to call chain for WMI event %u\n",
		       tm_priv->type);
}

static int lenovo_wmi_gz_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_wmi_gz_priv *priv =
		devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);

	if (!priv)
		return -ENOMEM;

	if (!context)
		return -EINVAL;

	priv->wdev = wdev;
	priv->type = *((enum lenovo_wmi_gz_type *)context);

	dev_set_drvdata(&wdev->dev, priv);

	if (priv->type != GAMEZONE_FULL)
		return 0;

	priv->nhead = gz_chain_head;
	priv->ppdev = platform_profile_register(&wdev->dev, "lenovo-wmi-gamezone",
						priv, &lenovo_wmi_gz_platform_profile_ops);

	return 0;
}

static const struct wmi_device_id lenovo_wmi_gz_id_table[] = {
	{ GAMEZONE_WMI_DEVICE(LENOVO_GAMEZONE_GUID, GAMEZONE_FULL) },
	{ GAMEZONE_WMI_DEVICE(THERMAL_MODE_EVENT_GUID, THERMAL_MODE) },
	{}
};

static struct wmi_driver lenovo_wmi_gz_driver = {
	.driver = {
		.name = "lenovo_wmi_gamezone",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lenovo_wmi_gz_id_table,
	.probe = lenovo_wmi_gz_probe,
	.notify = lenovo_wmi_gz_notify,
	.no_singleton = true,
};

module_wmi_driver(lenovo_wmi_gz_driver);

MODULE_IMPORT_NS("LENOVO_WMI");
MODULE_DEVICE_TABLE(wmi, lenovo_wmi_gz_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo GameZone WMI Driver");
MODULE_LICENSE("GPL");
