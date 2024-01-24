// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2016, Fuzhou Rockchip Electronics Co., Ltd
// Copyright (c) 2024 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
/*
 * Based on drivers/power/reset/reboot-mode.c
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power/power_on_reason.h>
#include <linux/pscrr.h>
#include <linux/reboot.h>

#define PREFIX "pscr-"

enum pscr system_pscr;

/*
 * struct pscr_to_magic_entry - Entry for mapping PSCR to magic values.
 *
 * This structure represents a single entry in a list that maps Power State
 * Change Reason (PSCR) values to corresponding magic values. Each entry
 * associates a specific PSCR with a unique magic value, facilitating easy
 * translation between the two.
 *
 * @pscr: The PSCR value.
 * @magic: The corresponding magic value.
 * @list: List head for chaining multiple such entries.
 */
struct pscr_to_magic_entry {
	enum pscr pscr;
	u32 magic;
	struct list_head list;
};

/*
 * struct pscr_map - Maps device tree property names to PSCR values.
 *
 * @dt_prop_name: Device tree property name without the "pscr-" prefix.
 * @pscr: The corresponding PSCR enum value for the given property name.
 */
struct pscr_map {
	const char *dt_prop_name;
	enum pscr pscr;
};

/*
 * struct pscr_map - Maps shortened DT property names to PSCR values.
 *
 * This structure maps device tree property names, with the "pscr-" prefix
 * omitted, to their corresponding Power State Change Reason (PSCR) values.
 */
struct pscr_map pscr_map_table[] = {
	{ "under-voltage", PSCR_UNDER_VOLTAGE },
	{ "over-current", PSCR_OVER_CURRENT },
	{ "regulator-failure", PSCR_REGULATOR_FAILURE },
	{ "over-temperature", PSCR_OVERTEMPERATURE },
};

/*
 * pscr_find_from_dt_name - Finds the PSCR value for a given DT property name.
 *
 * @dt_prop_name: The device tree property name, without the "pscr-" prefix, to
 * look up.
 * Returns the corresponding PSCR value or -ENOENT if not found.
 */
static int find_pscr_by_string(const char *dt_prop_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pscr_map_table); i++) {
		if (!strcmp(dt_prop_name, pscr_map_table[i].dt_prop_name))
			return pscr_map_table[i].pscr;
	}

	return -ENOENT;
}

static enum pscr get_pscr_by_magic(struct pscrr_device *pscrr_dev, u32 magic)
{
	struct pscr_to_magic_entry *map_entry;

	list_for_each_entry(map_entry, &pscrr_dev->pscr_map_list, list) {
		if (map_entry->magic == magic)
			return map_entry->pscr;
	}

	return 0;
}

static u32 get_magic_by_pscr(struct pscrr_device *pscrr_dev, enum pscr pscr)
{
	struct pscr_to_magic_entry *map_entry;

	list_for_each_entry(map_entry, &pscrr_dev->pscr_map_list, list) {
		if (map_entry->pscr == pscr)
			return map_entry->magic;
	}

	return 0;
}

/**
 * set_power_state_change_reason() - Set the system's power state change reason
 * @pscr: The enum value representing the power state change reason
 *
 * This function sets the system's power state change reason based on the
 * provided enum value.
 */
void set_power_state_change_reason(enum pscr pscr)
{
	system_pscr = pscr;
}
EXPORT_SYMBOL_GPL(set_power_state_change_reason);

static const char *pscr_to_por_string(enum pscr pscr)
{
	switch (pscr) {
	case PSCR_UNDER_VOLTAGE:
		return POWER_ON_REASON_BROWN_OUT;
	case PSCR_OVER_CURRENT:
		return POWER_ON_REASON_OVER_CURRENT;
	case PSCR_REGULATOR_FAILURE:
		return POWER_ON_REASON_REGULATOR_FAILURE;
	case PSCR_OVERTEMPERATURE:
		return POWER_ON_REASON_OVERTEMPERATURE;
	default:
	}

	return POWER_ON_REASON_UNKNOWN;
}

static ssize_t power_state_change_reason_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct pscrr_device *pscrr_dev = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", pscr_to_por_string(pscrr_dev->last_pscr));
}
static DEVICE_ATTR_RO(power_state_change_reason);

int handle_last_pscr(struct pscrr_device *pscrr_dev)
{
	u32 magic;
	int ret;

	ret = pscrr_dev->read(pscrr_dev, &magic);
	if (ret)
		return ret;

	pscrr_dev->last_pscr = get_pscr_by_magic(pscrr_dev, magic);

	dev_info(pscrr_dev->dev, "Last recorded power state change reason: %s\n",
		 pscr_to_por_string(pscrr_dev->last_pscr));

	ret = pscrr_dev->write(pscrr_dev, 0);
	if (ret)
		dev_err(pscrr_dev->dev, "Failed to clear power state change reason\n");

	return ret;
}

static int pscrr_notify(struct notifier_block *this, unsigned long x, void *c)
{
	struct pscrr_device *pscrr_dev = container_of(this, struct pscrr_device,
						      reboot_notifier);
	u32 magic;

	magic = get_magic_by_pscr(pscrr_dev, system_pscr);
	pscrr_dev->write(pscrr_dev, magic);

	return NOTIFY_DONE;
}

/**
 * pscrr_process_property() - Process a power state change reason property
 * @pscrr_dev: Pointer to the pscrr_device structure
 * @prop: Pointer to the property structure to be processed
 *
 * This function processes a device tree property representing a power state
 * change reason and initializes the relevant data structures.
 *
 * Returns: 0 on success, -ENOMEM on memory allocation failure.
 */
static int pscrr_process_property(struct pscrr_device *pscrr_dev,
				  struct property *prop)
{
	struct pscr_to_magic_entry *map_entry;
	struct device *dev = pscrr_dev->dev;
	size_t len = strlen(PREFIX);
	int ret;

	if (strncmp(prop->name, PREFIX, len))
		return 0;

	map_entry = devm_kzalloc(dev, sizeof(*map_entry), GFP_KERNEL);
	if (!map_entry)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, prop->name, &map_entry->magic);
	if (ret) {
		dev_err(dev, "Can't read magic number for %s: %pe\n",
			prop->name, ERR_PTR(ret));
		devm_kfree(dev, map_entry);
		return 0;
	}

	if (!map_entry->magic) {
		dev_err(dev, "%s with magic number == 0\n", prop->name);
		devm_kfree(dev, map_entry);
		return 0;
	}

	map_entry->pscr = find_pscr_by_string(prop->name + len);
	if (map_entry->pscr < 0) {
		dev_err(dev, "unsupported reason name(%s): %pe\n",
			prop->name, ERR_PTR(map_entry->pscr));
		devm_kfree(dev, map_entry);
		return 0;
	}

	if (map_entry->magic > pscrr_dev->max_magic_val)
		pscrr_dev->max_magic_val = map_entry->magic;

	dev_dbg(dev, "registering reason = %s, magic = %d, pscr = %d\n",
		prop->name, map_entry->magic, map_entry->pscr);
	list_add_tail(&map_entry->list, &pscrr_dev->pscr_map_list);

	return 0;
}

/*
 * pscrr_register() - Register the pscr driver and initialize power state change
 *                   reasons
 * @pscrr_dev: Pointer to the pscrr_device structure
 *
 * This function registers the pscr driver and initializes power state change
 * reasons based on device tree properties.
 *
 * Returns: 0 on success, -ENOMEM on memory allocation failure
 */
int pscrr_register(struct pscrr_device *pscrr_dev)
{
	struct device_node *np = pscrr_dev->dev->of_node;
	struct property *prop;
	int ret;

	INIT_LIST_HEAD(&pscrr_dev->pscr_map_list);

	for_each_property_of_node(np, prop) {
		ret = pscrr_process_property(pscrr_dev, prop);
		if (ret)
			return ret;
	}

	pscrr_dev->reboot_notifier.notifier_call = pscrr_notify;
	register_reboot_notifier(&pscrr_dev->reboot_notifier);

	dev_set_drvdata(pscrr_dev->dev, pscrr_dev);

	ret = device_create_file(pscrr_dev->dev, &dev_attr_power_state_change_reason);
	if (ret)
		dev_err(pscrr_dev->dev, "Could not create sysfs entry\n");

	return ret;
}
EXPORT_SYMBOL_GPL(pscrr_register);

/*
 * pscrr_unregister() - Unregister the pscr driver's reboot notifier
 * @pscrr_dev: Pointer to the pscrr_device structure
 *
 * This function unregisters the reboot notifier for the pscr driver.
 */
void pscrr_unregister(struct pscrr_device *pscrr_dev)
{
	unregister_reboot_notifier(&pscrr_dev->reboot_notifier);
}
EXPORT_SYMBOL_GPL(pscrr_unregister);

static void devm_pscrr_release(struct device *dev, void *res)
{
	pscrr_unregister(*(struct pscrr_device **)res);
}

/**
 * devm_pscrr_register - Register a device-managed PSCR driver
 * @dev: Device to associate the PSCR driver with
 * @pscrr_dev: Pointer to the PSCR driver to be registered
 *
 * Registers a Power State Change Reason (PSCR) driver as a device-managed
 * resource.
 *
 * Returns: 0 on successful registration or a negative error code on failure.
 */
int devm_pscrr_register(struct device *dev, struct pscrr_device *pscrr_dev)
{
	struct pscrr_device **dr;
	int rc;

	dr = devres_alloc(devm_pscrr_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	rc = pscrr_register(pscrr_dev);
	if (rc) {
		devres_free(dr);
		return rc;
	}

	*dr = pscrr_dev;
	devres_add(dev, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_pscrr_register);

static int devm_pscrr_match(struct device *dev, void *res, void *data)
{
	struct pscrr_device **p = res;

	if (WARN_ON(!p || !*p))
		return 0;

	return *p == data;
}

/**
 * devm_pscrr_unregister - Unregister a managed PSCR driver
 * @dev: Device associated with the PSCR driver
 * @pscrr_dev: Pointer to the PSCR driver to unregister
 *
 * Unregisters a device-managed Power State Change Reason (PSCR) driver.
 * It handles the cleanup and release of resources associated with the PSCR
 * driver which was previously registered.
 */
void devm_pscrr_unregister(struct device *dev,
			   struct pscrr_device *pscrr_dev)
{
	WARN_ON(devres_release(dev,
			       devm_pscrr_release,
			       devm_pscrr_match, pscrr_dev));
}
EXPORT_SYMBOL_GPL(devm_pscrr_unregister);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("Power State Change Reason (PSCR) Recording framework");
MODULE_LICENSE("GPL");
