// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LENOVO_CAPABILITY_DATA_01 WMI data block driver. This interface provides
 * information on tunable attributes used by the "Other Method" WMI interface,
 * including if it is supported by the hardware, the default_value, max_value,
 * min_value, and step increment.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/list.h>
#include "lenovo-wmi.h"

#define LENOVO_CAPABILITY_DATA_01_GUID "7A8F5407-CB67-4D6E-B547-39B3BE018154"

static DEFINE_MUTEX(cd01_call_mutex);
static DEFINE_MUTEX(cd01_list_mutex);
static LIST_HEAD(cd01_wmi_list);

static const struct wmi_device_id lenovo_wmi_capdata01_id_table[] = {
	{ LENOVO_CAPABILITY_DATA_01_GUID, NULL },
	{}
};

struct lenovo_wmi_cd01_priv {
	struct wmi_device *wdev;
	struct list_head list;
};

static inline struct lenovo_wmi_cd01_priv *get_first_wmi_priv(void)
{
	guard(mutex)(&cd01_list_mutex);
	return list_first_entry_or_null(&cd01_wmi_list,
					struct lenovo_wmi_cd01_priv, list);
}

int lenovo_wmi_capdata01_get(struct lenovo_wmi_attr_id attr_id,
			     struct capability_data_01 *cap_data)
{
	u32 attribute_id = *(int *)&attr_id;
	struct lenovo_wmi_cd01_priv *priv;
	union acpi_object *ret_obj;
	int instance_idx;
	int count;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	guard(mutex)(&cd01_call_mutex);
	count = wmidev_instance_count(priv->wdev);
	pr_info("Got instance count: %u\n", count);

	for (instance_idx = 0; instance_idx < count; instance_idx++) {
		ret_obj = wmidev_block_query(priv->wdev, instance_idx);
		if (!ret_obj) {
			pr_err("WMI Data block query failed.\n");
			continue;
		}

		if (ret_obj->type != ACPI_TYPE_BUFFER) {
			pr_err("WMI Data block query returned wrong type.\n");
			kfree(ret_obj);
			continue;
		}

		if (ret_obj->buffer.length != sizeof(*cap_data)) {
			pr_err("WMI Data block query returned wrong buffer length: %u vice expected %lu.\n",
			       ret_obj->buffer.length, sizeof(*cap_data));
			kfree(ret_obj);
			continue;
		}

		memcpy(cap_data, ret_obj->buffer.pointer,
		       ret_obj->buffer.length);
		kfree(ret_obj);

		if (cap_data->id != attribute_id)
			continue;
		break;
	}

	if (cap_data->id != attribute_id) {
		pr_err("Unable to find capability data for attribute_id %x\n",
		       attribute_id);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_capdata01_get, "CAPDATA_WMI");

static int lenovo_wmi_capdata01_probe(struct wmi_device *wdev,
				      const void *context)

{
	struct lenovo_wmi_cd01_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;

	guard(mutex)(&cd01_list_mutex);
	list_add_tail(&priv->list, &cd01_wmi_list);

	return 0;
}

static void lenovo_wmi_capdata01_remove(struct wmi_device *wdev)
{
	struct lenovo_wmi_cd01_priv *priv = dev_get_drvdata(&wdev->dev);

	guard(mutex)(&cd01_list_mutex);
	list_del(&priv->list);
}

static struct wmi_driver lenovo_wmi_capdata01_driver = {
	.driver = { .name = "lenovo_wmi_capdata01" },
	.id_table = lenovo_wmi_capdata01_id_table,
	.probe = lenovo_wmi_capdata01_probe,
	.remove = lenovo_wmi_capdata01_remove,
};

module_wmi_driver(lenovo_wmi_capdata01_driver);

MODULE_DEVICE_TABLE(wmi, lenovo_wmi_capdata01_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Capability Data 01 WMI Driver");
MODULE_LICENSE("GPL");
