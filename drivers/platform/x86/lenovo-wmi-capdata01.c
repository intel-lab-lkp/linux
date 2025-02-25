// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LENOVO_CAPABILITY_DATA_01 WMI data block driver. This interface provides
 * information on tunable attributes used by the "Other Mode" WMI interface,
 * including if it is supported by the hardware, the default_value, max_value,
 * min_value, and step increment.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/cleanup.h>
#include <linux/component.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/gfp_types.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include "lenovo-wmi.h"

/* Interface GUIDs */
#define LENOVO_CAPABILITY_DATA_01_GUID "7A8F5407-CB67-4D6E-B547-39B3BE018154"

static int lenovo_cd01_component_bind(struct device *cd01_dev,
				      struct device *om_dev, void *data)
{
	struct lenovo_wmi_cd01 *cd01 = dev_get_drvdata(cd01_dev);
	struct lenovo_wmi_om *om = dev_get_drvdata(om_dev);

	om->cd01 = cd01;
	return 0;
}

static void lenovo_cd01_component_unbind(struct device *cd01_dev,
					 struct device *om_dev, void *data)

{
	struct wmi_device *om_wdev =
		container_of(om_dev, struct wmi_device, dev);
	struct lenovo_wmi_om *om =
		container_of(&om_wdev, struct lenovo_wmi_om, wdev);

	om->cd01 = NULL;
}

static const struct component_ops lenovo_cd01_component_ops = {
	.bind = lenovo_cd01_component_bind,
	.unbind = lenovo_cd01_component_unbind,
};

static int lenovo_wmi_cd01_setup(struct lenovo_wmi_cd01 *cd01)
{
	size_t cd_size = sizeof(struct capdata01);
	int count, idx;

	count = wmidev_instance_count(cd01->wdev);

	cd01->capdata = devm_kmalloc_array(&cd01->wdev->dev, (size_t)count,
					   sizeof(*cd01->capdata), GFP_KERNEL);
	if (!cd01->capdata)
		return -ENOMEM;

	cd01->instance_count = count;

	for (idx = 0; idx < count; idx++) {
		union acpi_object *ret_obj __free(kfree) = NULL;
		struct capdata01 *cap_ptr =
			devm_kmalloc(&cd01->wdev->dev, cd_size, GFP_KERNEL);
		ret_obj = wmidev_block_query(cd01->wdev, idx);
		if (!ret_obj)
			continue;

		if (ret_obj->type != ACPI_TYPE_BUFFER)
			continue;

		if (ret_obj->buffer.length != cd_size)
			continue;

		memcpy(cap_ptr, ret_obj->buffer.pointer,
		       ret_obj->buffer.length);
		cd01->capdata[idx] = cap_ptr;
	}
	return 0;
}

static int lenovo_wmi_cd01_probe(struct wmi_device *wdev, const void *context)

{
	struct lenovo_wmi_cd01 *cd01;
	int ret;

	cd01 = devm_kzalloc(&wdev->dev, sizeof(*cd01), GFP_KERNEL);
	if (!cd01)
		return -ENOMEM;

	cd01->wdev = wdev;

	ret = lenovo_wmi_cd01_setup(cd01);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, cd01);

	ret = component_add(&wdev->dev, &lenovo_cd01_component_ops);

	return ret;
}

static void lenovo_wmi_cd01_remove(struct wmi_device *wdev)
{
	component_del(&wdev->dev, &lenovo_cd01_component_ops);
}

static const struct wmi_device_id lenovo_wmi_cd01_id_table[] = {
	{ LENOVO_CAPABILITY_DATA_01_GUID, NULL },
	{}
};

static struct wmi_driver lenovo_wmi_cd01_driver = {
	.driver = {
		.name = "lenovo_wmi_cd01",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lenovo_wmi_cd01_id_table,
	.probe = lenovo_wmi_cd01_probe,
	.remove = lenovo_wmi_cd01_remove,
	.no_singleton = true,
};

int lenovo_wmi_cd01_match(struct device *dev, void *data)
{
	return dev->driver == &lenovo_wmi_cd01_driver.driver;
}
EXPORT_SYMBOL_GPL(lenovo_wmi_cd01_match);

module_wmi_driver(lenovo_wmi_cd01_driver);

MODULE_DEVICE_TABLE(wmi, lenovo_wmi_cd01_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Capability Data 01 WMI Driver");
MODULE_LICENSE("GPL");
