// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Legion WMI interface driver. The Lenovo Legion WMI interface is
 * broken up into multiple GUID interfaces that require cross-references
 * between GUID's for some functionality. The "Custom Method" interface is a
 * legacy interface for managing and displaying CPU & GPU power and hwmon
 * settings and readings. The "Other Mode" interface is a modern interface
 * that replaces or extends the "Custom Method" interface methods. The
 * "Gamezone" interface adds advanced features such as fan profiles and
 * overclocking. The "Lighting" interface adds control of various status
 * lights related to different hardware components. "Other Mode" uses
 * the data structs LENOVO_CAPABILITY_DATA_00, LENOVO_CAPABILITY_DATA_01
 * and LENOVO_CAPABILITY_DATA_02 structs for capability information.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */

#include <linux/wmi.h>
#include "lenovo-wmi.h"

/* wmidev_evaluate_method helper functions */
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
				    u32 *retval)
{
	struct wmi_method_args args = { arg0, arg1 };
	struct acpi_buffer input = { (acpi_size)sizeof(args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *ret_obj __free(kfree) = NULL;
	int err;

	err = lenovo_wmidev_evaluate_method(wdev, instance, method_id, &input,
					    &output);

	if (err)
		return err;

	if (retval) {
		ret_obj = (union acpi_object *)output.pointer;
		if (!ret_obj)
			return -ENODATA;

		if (ret_obj->type != ACPI_TYPE_INTEGER)
			return -ENXIO;

		*retval = (u32)ret_obj->integer.value;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmidev_evaluate_method_2, "LENOVO_WMI");

int lenovo_wmidev_evaluate_method_1(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 *retval)
{
	return lenovo_wmidev_evaluate_method_2(wdev, instance, method_id, arg0,
					       0, retval);
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmidev_evaluate_method_1, "LENOVO_WMI");
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo WMI Common Driver");
MODULE_LICENSE("GPL");
