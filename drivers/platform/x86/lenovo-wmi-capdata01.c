// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Capability Data 01 WMI Data Block driver.
 *
 * Lenovo Capability Data 01 provides information on tunable attributes used by
 * the "Other Mode" WMI interface. The data includes if the attribute is
 * supported by the hardware, the default_value, max_value, min_value, and step
 * increment. Each attibute has multiple pages, one for each of the thermal
 * modes managed by the Gamezone interface.
 *
 * Copyright(C) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/component.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/gfp_types.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include "lenovo-wmi-capdata01.h"

#define LENOVO_CAPABILITY_DATA_01_GUID "7A8F5407-CB67-4D6E-B547-39B3BE018154"

struct lwmi_cd01_priv {
	struct wmi_device *wdev;
	struct cd01_list *list;
};

/**
 * lwmi_cd01_component_bind() - Bind component to master device.
 * @cd01_dev: Pointer to the lenovo-wmi-capdata01 driver parent device.
 * @om_dev: Pointer to the lenovo-wmi-other driver parent device.
 * @data: capdata01_list object pointer used to return the capability data.
 *
 * On lenovo-wmi-other's master bind, provide a pointer to the local capdata01
 * list. This is used to look up attribute data by the lenovo-wmi-other driver.
 *
 * Return: 0 on success, or on error.
 */
static int lwmi_cd01_component_bind(struct device *cd01_dev,
				    struct device *om_dev, void *data)
{
	struct lwmi_cd01_priv *priv = dev_get_drvdata(cd01_dev);
	struct cd01_list **cd01_list = data;

	if (!priv->list)
		return -ENODEV;

	*cd01_list = priv->list;

	return 0;
}

static const struct component_ops lwmi_cd01_component_ops = {
	.bind = lwmi_cd01_component_bind,
};

/**
 * lwmi_cd01_setup() - Cache all WMI data block information
 * @priv: lenovo-wmi-capdata01 driver data.
 *
 * Allocate a cd01_list struct large enough to contain data from all WMI data
 * blocks provided by the interface. Then loop through each data block and
 * cache the data.
 *
 * Return: 0 on success, or on error.
 */
static int lwmi_cd01_setup(struct lwmi_cd01_priv *priv)
{
	struct cd01_list *list;
	size_t list_size;
	int count, idx;

	count = wmidev_instance_count(priv->wdev);
	list_size = struct_size(list, data, count);

	list = devm_kzalloc(&priv->wdev->dev, list_size, GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	list->count = count;

	for (idx = 0; idx < count; idx++) {
		union acpi_object *ret_obj __free(kfree) = NULL;

		ret_obj = wmidev_block_query(priv->wdev, idx);
		if (!ret_obj)
			return -ENODEV;

		if (ret_obj->type != ACPI_TYPE_BUFFER ||
		    ret_obj->buffer.length < sizeof(struct capdata01))
			continue;

		memcpy(&list->data[idx], ret_obj->buffer.pointer,
		       ret_obj->buffer.length);
	}

	priv->list = list;

	return 0;
}

static int lwmi_cd01_probe(struct wmi_device *wdev, const void *context)

{
	struct lwmi_cd01_priv *priv;
	int ret;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	ret = lwmi_cd01_setup(priv);
	if (ret)
		return ret;

	return component_add(&wdev->dev, &lwmi_cd01_component_ops);
}

static void lwmi_cd01_remove(struct wmi_device *wdev)
{
	component_del(&wdev->dev, &lwmi_cd01_component_ops);
}

static const struct wmi_device_id lwmi_cd01_id_table[] = {
	{ LENOVO_CAPABILITY_DATA_01_GUID, NULL },
	{}
};

static struct wmi_driver lwmi_cd01_driver = {
	.driver = {
		.name = "lenovo_wmi_cd01",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lwmi_cd01_id_table,
	.probe = lwmi_cd01_probe,
	.remove = lwmi_cd01_remove,
	.no_singleton = true,
};

/**
 * lwmi_cd01_match() - Match rule for the master driver.
 * @dev: Pointer to the capability data 01 parent device.
 * @data: Unused void pointer for passing match criteria.
 *
 * Return: bool.
 */
int lwmi_cd01_match(struct device *dev, void *data)
{
	return dev->driver == &lwmi_cd01_driver.driver;
}
EXPORT_SYMBOL_NS_GPL(lwmi_cd01_match, "LENOVO_WMI_CD01");

module_wmi_driver(lwmi_cd01_driver);

MODULE_DEVICE_TABLE(wmi, lwmi_cd01_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Capability Data 01 WMI Driver");
MODULE_LICENSE("GPL");
