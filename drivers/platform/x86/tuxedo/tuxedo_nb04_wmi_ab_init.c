// SPDX-License-Identifier: GPL-2.0
/*
 * This driver implements the WMI AB device found on TUXEDO Notebooks with board
 * vendor NB04.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/wmi.h>
#include <linux/dmi.h>

#include "tuxedo_nb04_wmi_ab_virtual_lamp_array.h"

#include "tuxedo_nb04_wmi_ab_init.h"

static int probe(struct wmi_device *wdev, const void __always_unused *context)
{
	struct tuxedo_nb04_wmi_driver_data_t *driver_data;
	int ret;

	driver_data = devm_kzalloc(&wdev->dev, sizeof(*driver_data), GFP_KERNEL);
	if (!driver_data)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, driver_data);

	ret = tuxedo_nb04_virtual_lamp_array_add_device(wdev,
							&driver_data->virtual_lamp_array_hdev);
	if (ret)
		return ret;

	return 0;
}

static void remove(struct wmi_device *wdev)
{
	struct tuxedo_nb04_wmi_driver_data_t *driver_data = dev_get_drvdata(&wdev->dev);

	hid_destroy_device(driver_data->virtual_lamp_array_hdev);
}

static const struct wmi_device_id tuxedo_nb04_wmi_ab_device_ids[] = {
	{ .guid_string = "80C9BAA6-AC48-4538-9234-9F81A55E7C85" },
	{ }
};
MODULE_DEVICE_TABLE(wmi, tuxedo_nb04_wmi_ab_device_ids);

static struct wmi_driver tuxedo_nb04_wmi_ab_driver = {
	.driver = {
		.name = "tuxedo_nb04_wmi_ab",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = tuxedo_nb04_wmi_ab_device_ids,
	.probe = probe,
	.remove = remove,
	.no_singleton = true,
};

// We don't know if the WMI API is stable and how unique the GUID is for this ODM. To be on the safe
// side we therefore only run this driver on tested devices defined by this list.
static const struct dmi_system_id tested_devices_dmi_table[] __initconst = {
	{
		// TUXEDO Sirius 16 Gen1
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "APX958"),
		},
	},
	{
		// TUXEDO Sirius 16 Gen2
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TUXEDO"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "AHP958"),
		},
	},
	{ }
};

static int __init tuxedo_nb04_wmi_ab_init(void)
{
	if (!dmi_check_system(tested_devices_dmi_table))
		return -ENODEV;

	return wmi_driver_register(&tuxedo_nb04_wmi_ab_driver);
}
module_init(tuxedo_nb04_wmi_ab_init);

static void __exit tuxedo_nb04_wmi_ab_exit(void)
{
	return wmi_driver_unregister(&tuxedo_nb04_wmi_ab_driver);
}
module_exit(tuxedo_nb04_wmi_ab_exit);

MODULE_DESCRIPTION("Virtual HID LampArray interface for TUXEDO NB04 devices");
MODULE_AUTHOR("Werner Sembach <wse@tuxedocomputers.com>");
MODULE_LICENSE("GPL");
