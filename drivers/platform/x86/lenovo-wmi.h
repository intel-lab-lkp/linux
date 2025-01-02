/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lenovo Legion WMI interface driver. The Lenovo Legion WMI interface is
 * broken up into multiple GUID interfaces that require cross-references
 * between GUID's for some functionality. The "Custom Mode" interface is a
 * legacy interface for managing and displaying CPU & GPU power and hwmon
 * settings and readings. The "Other Mode" interface is a modern interface
 * that replaces or extends the "Custom Mode" interface methods. The
 * "GameZone" interface adds advanced features such as fan profiles and
 * overclocking. The "Lighting" interface adds control of various status
 * lights related to different hardware components. "Other Method" uses
 * the data structs LENOVO_CAPABILITY_DATA_00, LENOVO_CAPABILITY_DATA_01
 * and LENOVO_CAPABILITY_DATA_02 structs for capability information.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#ifndef _LENOVO_WMI_H_
#define _LENOVO_WMI_H_

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wmi.h>

/* Platform Profile Modes */
#define SMARTFAN_MODE_QUIET 0x01
#define SMARTFAN_MODE_BALANCED 0x02
#define SMARTFAN_MODE_PERFORMANCE 0x03
#define SMARTFAN_MODE_CUSTOM 0xFF

struct wmi_method_args {
	u32 arg0;
	u32 arg1;
};

struct lenovo_wmi_attr_id {
	u32 mode_id : 16; /* Fan profile */
	u32 feature_id : 8; /* Attribute (SPL/SPPT/...) */
	u32 device_id : 8; /* CPU/GPU/... */
} __packed;

enum attribute_property {
	DEFAULT_VAL,
	MAX_VAL,
	MIN_VAL,
	STEP_VAL,
	SUPPORTED,
};

/* Data struct for LENOVO_CAPABILITY_DATA_01 */
struct capability_data_01 {
	u32 id;
	u32 supported;
	u32 default_value;
	u32 step;
	u32 min_value;
	u32 max_value;
};

/* Tunable attribute that uses LENOVO_CAPABILITY_DATA_01 */
struct tunable_attr_01 {
	struct capability_data_01 capdata;
	u32 device_id;
	u32 feature_id;
	u32 store_value;
};

/* General Use functions */
static int lenovo_wmidev_evaluate_method(struct wmi_device *wdev, u8 instance,
					 u32 method_id, struct acpi_buffer *in,
					 struct acpi_buffer *out)
{
	acpi_status status;

	status = wmidev_evaluate_method(wdev, instance, method_id, in, out);

	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
};

int lenovo_wmidev_evaluate_method_2(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 arg1,
				    u32 *retval);

int lenovo_wmidev_evaluate_method_2(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 arg1,
				    u32 *retval)
{
	struct wmi_method_args args = { arg0, arg1 };
	struct acpi_buffer input = { (acpi_size)sizeof(args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *ret_obj = NULL;
	int err;

	err = lenovo_wmidev_evaluate_method(wdev, instance, method_id, &input,
					    &output);

	if (err) {
		pr_err("Attempt to get method value failed.\n");
		return err;
	}

	if (retval) {
		ret_obj = (union acpi_object *)output.pointer;
		if (!ret_obj) {
			pr_err("Failed to get valid ACPI object from WMI interface\n");
			return -EIO;
		}
		if (ret_obj->type != ACPI_TYPE_INTEGER) {
			pr_err("WMI query returnd ACPI object with wrong type.\n");
			kfree(ret_obj);
			return -EIO;
		}
		*retval = (u32)ret_obj->integer.value;
	}

	kfree(ret_obj);

	return 0;
}

int lenovo_wmidev_evaluate_method_1(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 *retval);

int lenovo_wmidev_evaluate_method_1(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 *retval)
{
	return lenovo_wmidev_evaluate_method_2(wdev, instance, method_id, arg0,
					       0, retval);
}

/* LENOVO_CAPABILITY_DATA_01 exported functions */
int lenovo_wmi_capdata01_get(struct lenovo_wmi_attr_id attr_id,
			     struct capability_data_01 *cap_data);

/* Other Method attribute functions */
ssize_t attr_capdata01_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf, struct tunable_attr_01 *tunable_attr,
			    enum attribute_property prop);

ssize_t attr_current_value_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count,
				 struct tunable_attr_01 *tunable_attr);

ssize_t attr_current_value_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf,
				struct tunable_attr_01 *tunable_attr);

ssize_t int_type_show(struct kobject *kobj, struct kobj_attribute *attr,
		      char *buf);

ssize_t int_type_show(struct kobject *kobj, struct kobj_attribute *attr,
		      char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

/* Other Method attribute macros */
#define __LL_ATTR_RO(_func, _name)                                    \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _func##_##_name##_show,                       \
	}

#define __LL_ATTR_RO_AS(_name, _show)                                 \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _show,                                        \
	}

#define __LL_ATTR_RW(_func, _name) \
	__ATTR(_name, 0644, _func##_##_name##_show, _func##_##_name##_store)

/* Shows a formatted static variable */
#define __ATTR_SHOW_FMT(_prop, _attrname, _fmt, _val)                         \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sysfs_emit(buf, _fmt, _val);                           \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__LL_ATTR_RO(_attrname, _prop)

/* Attribute current_value show/store */
#define __LL_TUNABLE_RW_CAP01(_attrname)                                      \
	static ssize_t _attrname##_current_value_store(                       \
		struct kobject *kobj, struct kobj_attribute *attr,            \
		const char *buf, size_t count)                                \
	{                                                                     \
		return attr_current_value_store(kobj, attr, buf, count,       \
						&_attrname);                  \
	}                                                                     \
	static ssize_t _attrname##_current_value_show(                        \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return attr_current_value_show(kobj, attr, buf, &_attrname);  \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_current_value =       \
		__LL_ATTR_RW(_attrname, current_value)

/* Attribute property show only */
#define __LL_TUNABLE_RO_CAP01(_prop, _attrname, _prop_type)                   \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return attr_capdata01_show(kobj, attr, buf, &_attrname,       \
					   _prop_type);                       \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__LL_ATTR_RO(_attrname, _prop)

#define ATTR_GROUP_LL_TUNABLE_CAP01(_attrname, _fsname, _dispname)     \
	__LL_TUNABLE_RW_CAP01(_attrname);                              \
	__LL_TUNABLE_RO_CAP01(default_value, _attrname, DEFAULT_VAL);  \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);   \
	__LL_TUNABLE_RO_CAP01(max_value, _attrname, MAX_VAL);          \
	__LL_TUNABLE_RO_CAP01(min_value, _attrname, MIN_VAL);          \
	__LL_TUNABLE_RO_CAP01(scalar_increment, _attrname, STEP_VAL);  \
	static struct kobj_attribute attr_##_attrname##_type =         \
		__LL_ATTR_RO_AS(type, int_type_show);                  \
	static struct attribute *_attrname##_attrs[] = {               \
		&attr_##_attrname##_current_value.attr,                \
		&attr_##_attrname##_default_value.attr,                \
		&attr_##_attrname##_display_name.attr,                 \
		&attr_##_attrname##_max_value.attr,                    \
		&attr_##_attrname##_min_value.attr,                    \
		&attr_##_attrname##_scalar_increment.attr,             \
		&attr_##_attrname##_type.attr,                         \
		NULL,                                                  \
	};                                                             \
	static const struct attribute_group _attrname##_attr_group = { \
		.name = _fsname, .attrs = _attrname##_attrs            \
	}

#endif /* !_LENOVO_WMI_H_ */
