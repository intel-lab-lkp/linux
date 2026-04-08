// SPDX-License-Identifier: GPL-2.0

#include <linux/acpi.h>

__rust_helper struct acpi_device *rust_helper_to_acpi_device_node(struct fwnode_handle *fwnode)
{
	return to_acpi_device_node(fwnode);
}
