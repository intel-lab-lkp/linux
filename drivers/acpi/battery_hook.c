// SPDX-License-Identifier: GPL-2.0
/*
 * ACPI Battery Hooking API
 *
 * Provides a common registration and notification framework for drivers
 * that attach platform-specific properties or extensions to ACPI-registered
 * batteries (both ACPI Control Method batteries and Smart Battery Systems).
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <acpi/battery.h>

static LIST_HEAD(acpi_battery_list);
static LIST_HEAD(battery_hook_list);
static DEFINE_MUTEX(hook_mutex);

static void battery_hook_unregister_unlocked(struct acpi_battery_hook *hook)
{
	struct acpi_battery_hook_device *hook_dev;

	list_for_each_entry(hook_dev, &acpi_battery_list, list) {
		if (!hook->remove_battery(hook_dev->battery, hook))
			power_supply_changed(hook_dev->battery);
	}
	list_del_init(&hook->list);

	pr_info("hook unregistered: %s\n", hook->name);
}

void battery_hook_unregister(struct acpi_battery_hook *hook)
{
	mutex_lock(&hook_mutex);
	if (!list_empty(&hook->list))
		battery_hook_unregister_unlocked(hook);
	mutex_unlock(&hook_mutex);
}
EXPORT_SYMBOL_GPL(battery_hook_unregister);

void battery_hook_register(struct acpi_battery_hook *hook)
{
	struct acpi_battery_hook_device *hook_dev;

	mutex_lock(&hook_mutex);
	list_add(&hook->list, &battery_hook_list);

	list_for_each_entry(hook_dev, &acpi_battery_list, list) {
		if (hook->add_battery(hook_dev->battery, hook)) {
			pr_err("hook failed to load: %s\n", hook->name);
			battery_hook_unregister_unlocked(hook);
			goto end;
		}

		power_supply_changed(hook_dev->battery);
	}
	pr_info("new hook: %s\n", hook->name);
end:
	mutex_unlock(&hook_mutex);
}
EXPORT_SYMBOL_GPL(battery_hook_register);

static void devm_battery_hook_unregister(void *data)
{
	struct acpi_battery_hook *hook = data;

	battery_hook_unregister(hook);
}

int devm_battery_hook_register(struct device *dev, struct acpi_battery_hook *hook)
{
	battery_hook_register(hook);

	return devm_add_action_or_reset(dev, devm_battery_hook_unregister, hook);
}
EXPORT_SYMBOL_GPL(devm_battery_hook_register);

void battery_hook_add_battery(struct acpi_battery_hook_device *hook_dev,
			      struct power_supply *battery)
{
	struct acpi_battery_hook *hook_node, *tmp;

	hook_dev->battery = battery;
	mutex_lock(&hook_mutex);
	INIT_LIST_HEAD(&hook_dev->list);
	list_add(&hook_dev->list, &acpi_battery_list);

	list_for_each_entry_safe(hook_node, tmp, &battery_hook_list, list) {
		if (hook_node->add_battery(hook_dev->battery, hook_node)) {
			pr_err("error in hook, unloading: %s\n", hook_node->name);
			battery_hook_unregister_unlocked(hook_node);
		}
	}
	mutex_unlock(&hook_mutex);
}
EXPORT_SYMBOL_GPL(battery_hook_add_battery);

void battery_hook_remove_battery(struct acpi_battery_hook_device *hook_dev)
{
	struct acpi_battery_hook *hook;

	mutex_lock(&hook_mutex);
	list_for_each_entry(hook, &battery_hook_list, list)
		hook->remove_battery(hook_dev->battery, hook);

	list_del(&hook_dev->list);
	hook_dev->battery = NULL;
	mutex_unlock(&hook_mutex);
}
EXPORT_SYMBOL_GPL(battery_hook_remove_battery);
