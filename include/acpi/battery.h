/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ACPI_BATTERY_H
#define __ACPI_BATTERY_H

#include <linux/device.h>
#include <linux/power_supply.h>

#define ACPI_BATTERY_CLASS "battery"

#define ACPI_BATTERY_NOTIFY_STATUS	0x80
#define ACPI_BATTERY_NOTIFY_INFO	0x81
#define ACPI_BATTERY_NOTIFY_THRESHOLD   0x82

struct acpi_battery_hook {
	const char *name;
	int (*add_battery)(struct power_supply *battery, struct acpi_battery_hook *hook);
	int (*remove_battery)(struct power_supply *battery, struct acpi_battery_hook *hook);
	struct list_head list;
};

/*
 * battery_hook_register() and friends only see batteries registered by
 * drivers/acpi/battery.c, the ACPI Control Method Battery driver (ACPI HID
 * "PNP0C0A"). Batteries registered by drivers/acpi/sbs.c, the ACPI Smart
 * Battery System driver (ACPI HID "ACPI0002", common on hardware with
 * SMBus/SBS fuel-gauge chips such as many Intel MacBooks), are invisible
 * to them; use the sbs_battery_hook_* equivalents below for those. A
 * caller wanting to support both kinds of hardware needs two separate
 * struct acpi_battery_hook instances, one per registration call, since a
 * given instance's embedded list node can only belong to one list at a
 * time.
 */
void battery_hook_register(struct acpi_battery_hook *hook);
void battery_hook_unregister(struct acpi_battery_hook *hook);
int devm_battery_hook_register(struct device *dev, struct acpi_battery_hook *hook);

void sbs_battery_hook_register(struct acpi_battery_hook *hook);
void sbs_battery_hook_unregister(struct acpi_battery_hook *hook);
int devm_sbs_battery_hook_register(struct device *dev, struct acpi_battery_hook *hook);

#endif
