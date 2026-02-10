// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Legion WMI helpers driver.
 *
 * The Lenovo Legion WMI interface is broken up into multiple GUID interfaces
 * that require cross-references between GUID's for some functionality. The
 * "Custom Mode" interface is a legacy interface for managing and displaying
 * CPU & GPU power and hwmon settings and readings. The "Other Mode" interface
 * is a modern interface that replaces or extends the "Custom Mode" interface
 * methods. The "Gamezone" interface adds advanced features such as fan
 * profiles and overclocking. The "Lighting" interface adds control of various
 * status lights related to different hardware components. Each of these
 * drivers uses a common procedure to get data from the WMI interface,
 * enumerated here.
 *
 * Copyright (C) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/cleanup.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#include "wmi-helpers.h"

/**
 * lwmi_dev_evaluate_int() - Helper function for calling WMI methods that
 * return an integer.
 * @wdev: Pointer to the WMI device to be called.
 * @instance: Instance of the called method.
 * @method_id: WMI Method ID for the method to be called.
 * @buf: Buffer of all arguments for the given method_id.
 * @size: Length of the buffer.
 * @retval: Pointer for the return value to be assigned.
 *
 * Calls wmidev_evaluate_method for Lenovo WMI devices that return an ACPI
 * integer. Validates the return value type and assigns the value to the
 * retval pointer.
 *
 * Return: 0 on success, or an error code.
 */
int lwmi_dev_evaluate_int(struct wmi_device *wdev, u8 instance, u32 method_id,
			  unsigned char *buf, size_t size, u32 *retval)
{
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *ret_obj __free(kfree) = NULL;
	struct acpi_buffer input = { size, buf };
	acpi_status status;

	status = wmidev_evaluate_method(wdev, instance, method_id, &input,
					&output);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (retval) {
		ret_obj = output.pointer;
		if (!ret_obj)
			return -ENODATA;

		switch (ret_obj->type) {
		/*
		 * The ACPI method may simply return a buffer when a u32
		 * is expected. This is valid on Windows as its WMI-ACPI
		 * driver converts everything to a common buffer.
		 */
		case ACPI_TYPE_BUFFER:
			if (ret_obj->buffer.length < sizeof(u32))
				return -ENXIO;

			*retval = get_unaligned_le32(ret_obj->buffer.pointer);
			return 0;
		case ACPI_TYPE_INTEGER:
			*retval = (u32)ret_obj->integer.value;
			return 0;
		default:
			return -ENXIO;
		}
	}

	return 0;
};
EXPORT_SYMBOL_NS_GPL(lwmi_dev_evaluate_int, "LENOVO_WMI_HELPERS");

static struct dentry *lwmi_debugfs_dir;

/**
 * lwmi_debugfs_create_dir() - Helper function for creating a debugfs directory
 * for a device.
 * @wdev: Pointer to the WMI device to be called.
 *
 * Caller must remove the directory with debugfs_remove_recursive() on device
 * removal.
 *
 * Return: Pointer to the created directory.
 */
struct dentry *lwmi_debugfs_create_dir(struct wmi_device *wdev)
{
	return debugfs_create_dir(dev_name(&wdev->dev), lwmi_debugfs_dir);
}
EXPORT_SYMBOL_NS_GPL(lwmi_debugfs_create_dir, "LENOVO_WMI_HELPERS");

static int __init lwmi_helpers_init(void)
{
	lwmi_debugfs_dir = debugfs_create_dir("lenovo_wmi", NULL);

	return 0;
}
module_init(lwmi_helpers_init)

static void __exit lwmi_helpers_exit(void)
{
	debugfs_remove_recursive(lwmi_debugfs_dir);
}
module_exit(lwmi_helpers_exit)

MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo WMI Helpers Driver");
MODULE_LICENSE("GPL");
