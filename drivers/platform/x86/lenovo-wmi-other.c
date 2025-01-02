// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Other Method WMI interface driver. This driver uses the fw_attributes
 * class to expose the various WMI functions provided by the "Other Method" WMI
 * interface. This enables CPU and GPU power limit as well as various other
 * attributes for devices that fall under the "Gaming Series" of Lenovo laptop
 * devices. Each attribute exposed by the "Other Method"" interface has a
 * corresponding LENOVO_CAPABILITY_DATA_01 struct that allows the driver to
 * probe details about the attribute such as set/get support, step, min, max,
 * and default value. Each attibute has multiple pages, one for each of the
 * fan profiles managed by the GameZone interface, so it must be probed prior
 * to returning the current_value.
 *
 * These attributes typically don't fit anywhere else in the sysfs and are set
 * in Windows using one of Lenovo's multiple user applications.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/list.h>
#include "lenovo-wmi.h"
#include "firmware_attributes_class.h"

#define FW_ATTR_FOLDER "lenovo-wmi-other"
#define LENOVO_OTHER_METHOD_GUID "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"

/* Device IDs */
#define WMI_DEVICE_ID_CPU 0x01

/* WMI_DEVICE_ID_CPU feature IDs */
#define WMI_FEATURE_ID_CPU_SPPT 0x01 /* Short Term Power Limit */
#define WMI_FEATURE_ID_CPU_FPPT 0x03 /* Long Term Power Limit */
#define WMI_FEATURE_ID_CPU_SPL 0x02 /* Peak Power Limit */
#define WMI_FEATURE_ID_CPU_FPPT_BAD 0x03 /* Long Term Power Limit */

/* Method IDs */
#define WMI_METHOD_ID_VALUE_GET 17 /* Other Method Getter */
#define WMI_METHOD_ID_VALUE_SET 18 /* Other Method Setter */

static DEFINE_MUTEX(call_mutex);
static DEFINE_MUTEX(om_list_mutex);
static LIST_HEAD(om_wmi_list);

struct lenovo_wmi_om_priv {
	struct wmi_device *wdev;
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;
	struct list_head list;
};

static inline struct lenovo_wmi_om_priv *get_first_wmi_priv(void)
{
	guard(mutex)(&om_list_mutex);
	return list_first_entry_or_null(&om_wmi_list, struct lenovo_wmi_om_priv,
					list);
}

static const struct wmi_device_id lenovo_wmi_other_id_table[] = {
	{ LENOVO_OTHER_METHOD_GUID, NULL },
	{}
};

/* Tunable Attributes */
struct tunable_attr_01 ppt_pl1_spl = { .device_id = WMI_DEVICE_ID_CPU,
				       .feature_id = WMI_FEATURE_ID_CPU_SPL };
struct tunable_attr_01 ppt_pl2_sppt = { .device_id = WMI_DEVICE_ID_CPU,
					.feature_id = WMI_FEATURE_ID_CPU_SPPT };
struct tunable_attr_01 ppt_pl3_fppt = { .device_id = WMI_DEVICE_ID_CPU,
					.feature_id = WMI_FEATURE_ID_CPU_FPPT };

struct capdata01_attr_group {
	const struct attribute_group *attr_group;
	struct tunable_attr_01 *tunable_attr;
};

static const struct class *fw_attr_class;

/**
 * attr_capdata01_setup() - Get the data of the specified attribute
 * from LENOVO_CAPABILITY_DATA_01 and store it.
 * @tunable_attr: The attribute to be populated.
 *
 * Returns: Either 0 or an error.
 */
static int attr_capdata01_setup(struct tunable_attr_01 *tunable_attr)
{
	struct capability_data_01 cap_data;
	int mode = SMARTFAN_MODE_CUSTOM;
	int err;

	struct lenovo_wmi_attr_id attr_id = { mode << 8,
					      tunable_attr->feature_id,
					      tunable_attr->device_id };

	err = lenovo_wmi_capdata01_get(attr_id, &cap_data);
	if (err) {
		pr_err("Failed to get capability data: %u\n", err);
		return err;
	}

	if (cap_data.supported < 1)
		return -EOPNOTSUPP;

	tunable_attr->capdata = cap_data;

	return 0;
}

/**
 * attr_capdata01_show() - Get the value of the specified attribute property
 * from LENOVO_CAPABILITY_DATA_01.
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to write to.
 * @tunable_attr: The attribute to be read.
 * @prop: The property of this attribute to be read.
 *
 * This function is intended to be generic so it can be called from any "_show"
 * attribute which works only with integers.
 *
 * If the WMI is success, then the sysfs attribute is notified.
 *
 * Returns: Either count, or an error.
 */
ssize_t attr_capdata01_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf, struct tunable_attr_01 *tunable_attr,
			    enum attribute_property prop)
{
	struct capability_data_01 cap_data;
	int retval;

	cap_data = tunable_attr->capdata;

	switch (prop) {
	case DEFAULT_VAL:
		retval = cap_data.default_value;
		break;
	case MAX_VAL:
		retval = cap_data.max_value;
		break;
	case MIN_VAL:
		retval = cap_data.min_value;
		break;
	case STEP_VAL:
		retval = cap_data.step;
		break;
	default:
		return -EINVAL;
	}
	return sysfs_emit(buf, "%u\n", retval);
}

/* Simple attribute creation */

/*
 * att_current_value_store() - Set the current value of the given attribute
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to read from, this is parsed to `int` type.
 * @count: Required by sysfs attribute macros, pass in from the callee attr.
 * @tunable_attr: The attribute to be stored.
 *
 * This function is intended to be generic so it can be called from any
 * attribute's "current_value_store" which works only with integers. The
 * integer to be sent to the WMI method is range checked and an error returned
 * if out of range.
 *
 * If the value is valid and WMI is success, then the sysfs attribute is
 * notified.
 *
 * Returns: Either count, or an error.
 */
ssize_t attr_current_value_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count,
				 struct tunable_attr_01 *tunable_attr)
{
	struct capability_data_01 cap_data;
	struct lenovo_wmi_om_priv *priv;
	int mode = SMARTFAN_MODE_CUSTOM;
	u32 value;
	int err;

	struct lenovo_wmi_attr_id attr_id = { mode << 8,
					      tunable_attr->feature_id,
					      tunable_attr->device_id };

	err = kstrtouint(buf, 10, &value);
	if (err) {
		pr_err("Error converting value to int: %u\n", err);
		return err;
	}

	cap_data = tunable_attr->capdata;

	if (value < cap_data.min_value || value > cap_data.max_value)
		return -EINVAL;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	guard(mutex)(&call_mutex);
	err = lenovo_wmidev_evaluate_method_2(priv->wdev, 0x0,
					      WMI_METHOD_ID_VALUE_SET,
					      *(int *)&attr_id, value, NULL);

	if (err) {
		pr_err("Error setting attribute: %u\n", err);
		return err;
	}

	tunable_attr->store_value = value;

	sysfs_notify(kobj, NULL, attr->attr.name);

	return count;
};

/*
 * attr_current_value_show() - Get the current value of the given attribute
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to write to.
 * @tunable_attr: The attribute to be read.
 *
 * This function is intended to be generic so it can be called from any "_show"
 * attribute which works only with integers.
 *
 * If the WMI is success, then the sysfs attribute is notified.
 *
 * Returns: Either count, or an error.
 */
ssize_t attr_current_value_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf,
				struct tunable_attr_01 *tunable_attr)
{
	struct lenovo_wmi_om_priv *priv;
	int mode = SMARTFAN_MODE_CUSTOM;
	int retval;
	int err;

	struct lenovo_wmi_attr_id attr_id = { mode << 8,
					      tunable_attr->feature_id,
					      tunable_attr->device_id };

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	guard(mutex)(&call_mutex);
	err = lenovo_wmidev_evaluate_method_1(priv->wdev, 0x0,
					      WMI_METHOD_ID_VALUE_GET,
					      *(int *)&attr_id, &retval);

	if (err) {
		pr_err("Error getting attribute: %u\n", err);
		return err;
	}

	return sysfs_emit(buf, "%u\n", retval);
}

ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl1_spl, "ppt_pl1_spl",
			    "Set the CPU sustained power limit");
ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl2_sppt, "ppt_pl2_sppt",
			    "Set the CPU slow package power tracking limit");
ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl3_fppt, "ppt_pl3_fppt",
			    "Set the CPU fast package power tracking limit");

static const struct capdata01_attr_group capdata01_attr_groups[] = {
	{ &ppt_pl1_spl_attr_group, &ppt_pl1_spl },
	{ &ppt_pl2_sppt_attr_group, &ppt_pl2_sppt },
	{ &ppt_pl3_fppt_attr_group, &ppt_pl3_fppt },
	{},
};

static int other_method_fw_attr_add(struct lenovo_wmi_om_priv *priv)
{
	int err, i;

	err = fw_attributes_class_get(&fw_attr_class);
	if (err) {
		pr_err("Failed to get firmware_attributes_class: %u\n", err);
		return err;
	}

	priv->fw_attr_dev = device_create(fw_attr_class, NULL, MKDEV(0, 0),
					  NULL, "%s", FW_ATTR_FOLDER);
	if (IS_ERR(priv->fw_attr_dev)) {
		err = PTR_ERR(priv->fw_attr_dev);
		pr_err("Failed to create firmware_attributes_class device: %u\n",
		       err);
		goto fail_class_get;
	}

	priv->fw_attr_kset = kset_create_and_add("attributes", NULL,
						 &priv->fw_attr_dev->kobj);
	if (!priv->fw_attr_kset) {
		err = -ENOMEM;
		pr_err("Failed to create firmware_attributes_class kset: %u\n",
		       err);
		goto err_destroy_classdev;
	}

	for (i = 0; i < ARRAY_SIZE(capdata01_attr_groups) - 1; i++) {
		err = attr_capdata01_setup(
			capdata01_attr_groups[i].tunable_attr);
		if (err) {
			pr_err("Failed to populate capability data for %s: %u\n",
			       capdata01_attr_groups[i].attr_group->name, err);
			continue;
		}

		err = sysfs_create_group(&priv->fw_attr_kset->kobj,
					 capdata01_attr_groups[i].attr_group);
		if (err) {
			pr_err("Failed to create sysfs-group for %s: %u\n",
			       capdata01_attr_groups[i].attr_group->name, err);
			goto err_remove_groups;
		}
	}

	return 0;

err_remove_groups:
	while (i-- > 0) {
		sysfs_remove_group(&priv->fw_attr_kset->kobj,
				   capdata01_attr_groups[i].attr_group);
	}

	return err;

err_destroy_classdev:
	device_destroy(fw_attr_class, MKDEV(0, 0));

	return err;

fail_class_get:
	fw_attributes_class_put();

	return err;
}

static int lenovo_wmi_other_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_wmi_om_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;

	guard(mutex)(&om_list_mutex);
	list_add_tail(&priv->list, &om_wmi_list);

	return other_method_fw_attr_add(priv);
}

static void lenovo_wmi_other_remove(struct wmi_device *wdev)
{
	struct lenovo_wmi_om_priv *priv = dev_get_drvdata(&wdev->dev);

	guard(mutex)(&om_list_mutex);
	list_del(&priv->list);
	kset_unregister(priv->fw_attr_kset);
	device_destroy(fw_attr_class, MKDEV(0, 0));
	fw_attributes_class_put();
}

static struct wmi_driver lenovo_wmi_other_driver = {
	.driver = { .name = "lenovo_wmi_other" },
	.id_table = lenovo_wmi_other_id_table,
	.probe = lenovo_wmi_other_probe,
	.remove = lenovo_wmi_other_remove,
};

module_wmi_driver(lenovo_wmi_other_driver);

MODULE_IMPORT_NS("CAPDATA_WMI");
MODULE_DEVICE_TABLE(wmi, lenovo_wmi_other_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Other Method WMI Driver");
MODULE_LICENSE("GPL");
