// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026, ZTE Corporation
 *  Author: Yu Ye Hu <hu.yuye@zte.com.cn>
 */
#include <linux/acpi.h>

void arch_acpi_pci_root_add_clear_dep(struct acpi_device *device)
{
	acpi_dev_clear_dependencies(device);
}
