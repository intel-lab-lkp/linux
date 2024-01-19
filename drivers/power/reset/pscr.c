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
#include <linux/reboot.h>
#include <linux/pscr.h>

#define PREFIX "pscr-"

struct reason_info {
	enum power_state_change_reason pscr;
	u32 magic;
	struct list_head list;
};

enum power_state_change_reason system_pscr;

struct pscr_map {
	const char *reason;
	enum power_state_change_reason pscr;
};

struct pscr_map pscr_map_table[] = {
	{ "unknown", PSCR_UNKNOWN },
	{ "under-voltage", PSCR_UNDER_VOLTAGE },
	{ "over-current", PSCR_OVER_CURRENT },
	{ "regulator-failure", PSCR_REGULATOR_FAILURE },
	{ "over-temperature", PSCR_OVERTEMPERATURE },
};

static int find_reason_by_string(const char *reason)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pscr_map_table); i++) {
		if (!strcmp(reason, pscr_map_table[i].reason))
			return pscr_map_table[i].pscr;
	}

	return -ENOENT;
}

/**
 * set_power_state_change_reason() - Set the system's power state change reason
 * @reason: The enum value representing the power state change reason
 *
 * This function sets the system's power state change reason based on the
 * provided enum value.
 */
void set_power_state_change_reason(enum power_state_change_reason reason)
{
	system_pscr = reason;
}
EXPORT_SYMBOL_GPL(set_power_state_change_reason);

static unsigned int get_pscr_magic(struct pscr_driver *pscr_drv,
					  const char *cmd)
{
	struct reason_info *info;

	list_for_each_entry(info, &pscr_drv->head, list) {
		if (info->pscr == system_pscr)
			return info->magic;
	}

	return 0;
}

static int pscr_notify(struct notifier_block *this,
			      unsigned long reason, void *cmd)
{
	struct pscr_driver *pscr_drv = container_of(this, struct pscr_driver,
						    reboot_notifier);
	unsigned int magic;

	magic = get_pscr_magic(pscr_drv, cmd);
	if (magic)
		pscr_drv->write(pscr_drv, magic);

	return NOTIFY_DONE;
}

/**
 * pscr_process_property() - Process a power state change reason property
 * @pscr_drv: Pointer to the pscr_driver structure
 * @prop: Pointer to the property structure to be processed
 *
 * This function processes a device tree property representing a power state
 * change reason and initializes the relevant data structures.
 *
 * Returns: 0 on success, -ENOMEM on memory allocation failure.
 */
static int pscr_process_property(struct pscr_driver *pscr_drv,
				 struct property *prop)
{
	struct device *dev = pscr_drv->dev;
	size_t len = strlen(PREFIX);
	struct reason_info *info;
	int ret;

	if (strncmp(prop->name, PREFIX, len))
		return 0;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, prop->name, &info->magic);
	if (ret) {
		dev_err(dev, "Can't read magic number for %s: %pe\n",
			prop->name, ERR_PTR(ret));
		devm_kfree(dev, info);
		return 0;
	}

	if (!info->magic) {
		dev_err(dev, "%s with magic number == 0\n", prop->name);
		devm_kfree(dev, info);
		return 0;
	}

	info->pscr = find_reason_by_string(prop->name + len);
	if (info->pscr < 0) {
		dev_err(dev, "unsupported reason name(%s): %pe\n",
				prop->name, ERR_PTR(info->pscr));
		devm_kfree(dev, info);
		return 0;
	}

	if (info->magic > pscr_drv->max_magic)
		pscr_drv->max_magic = info->magic;

	dev_dbg(dev, "registering reason = %s, magic = %d, pscr = %d\n",
		prop->name, info->magic, info->pscr);
	list_add_tail(&info->list, &pscr_drv->head);

	return 0;
}

/*
 * pscr_register() - Register the pscr driver and initialize power state change
 *                   reasons
 * @pscr_drv: Pointer to the pscr_driver structure
 *
 * This function registers the pscr driver and initializes power state change
 * reasons based on device tree properties.
 *
 * Returns: 0 on success, -ENOMEM on memory allocation failure
 */
int pscr_register(struct pscr_driver *pscr_drv)
{
	struct device_node *np = pscr_drv->dev->of_node;
	struct property *prop;

	INIT_LIST_HEAD(&pscr_drv->head);

	for_each_property_of_node(np, prop) {
		int ret = pscr_process_property(pscr_drv, prop);
		if (ret)
			return ret;
	}

	pscr_drv->reboot_notifier.notifier_call = pscr_notify;
	register_reboot_notifier(&pscr_drv->reboot_notifier);

	return 0;
}
EXPORT_SYMBOL_GPL(pscr_register);

/*
 * pscr_unregister() - Unregister the pscr driver's reboot notifier
 * @pscr_drv: Pointer to the pscr_driver structure
 *
 * This function unregisters the reboot notifier for the pscr driver.
 */
void pscr_unregister(struct pscr_driver *pscr_drv)
{
	unregister_reboot_notifier(&pscr_drv->reboot_notifier);
}
EXPORT_SYMBOL_GPL(pscr_unregister);

static void devm_pscr_release(struct device *dev, void *res)
{
	pscr_unregister(*(struct pscr_driver **)res);
}

/**
 * devm_pscr_register - Register a device-managed PSCR driver
 * @dev: Device to associate the PSCR driver with
 * @pscr_drv: Pointer to the PSCR driver to be registered
 *
 * Registers a Power State Change Reason (PSCR) driver as a device-managed
 * resource.
 *
 * Returns: 0 on successful registration or a negative error code on failure.
 */
int devm_pscr_register(struct device *dev,
			      struct pscr_driver *pscr_drv)
{
	struct pscr_driver **dr;
	int rc;

	dr = devres_alloc(devm_pscr_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	rc = pscr_register(pscr_drv);
	if (rc) {
		devres_free(dr);
		return rc;
	}

	*dr = pscr_drv;
	devres_add(dev, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_pscr_register);

static int devm_pscr_match(struct device *dev, void *res, void *data)
{
	struct pscr_driver **p = res;

	if (WARN_ON(!p || !*p))
		return 0;

	return *p == data;
}

/**
 * devm_pscr_unregister - Unregister a managed PSCR driver
 * @dev: Device associated with the PSCR driver
 * @pscr_drv: Pointer to the PSCR driver to unregister
 *
 * Unregisters a device-managed Power State Change Reason (PSCR) driver.
 * It handles the cleanup and release of resources associated with the PSCR
 * driver which was previously registered.
 */
void devm_pscr_unregister(struct device *dev,
				 struct pscr_driver *pscr_drv)
{
	WARN_ON(devres_release(dev,
			       devm_pscr_release,
			       devm_pscr_match, pscr_drv));
}
EXPORT_SYMBOL_GPL(devm_pscr_unregister);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("Power State Change Reason (PSCR) tracking framework");
MODULE_LICENSE("GPL v2");
