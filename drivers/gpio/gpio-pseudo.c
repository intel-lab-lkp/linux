// SPDX-License-Identifier: GPL-2.0+
/*
 * Helper functions for Pseudo GPIOs
 *
 * Copyright 2025 Canonical Ltd.
 */

#include "gpio-pseudo.h"

static int pseudo_gpio_notifier_call(struct notifier_block *nb,
				     unsigned long action,
				     void *data)
{
	struct pseudo_gpio_common *common;
	struct device *dev = data;

	common = container_of(nb, struct pseudo_gpio_common, bus_notifier);
	if (!device_match_name(dev, common->name))
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_BOUND_DRIVER:
		common->driver_bound = true;
		break;
	case BUS_NOTIFY_DRIVER_NOT_BOUND:
		common->driver_bound = false;
		break;
	default:
		return NOTIFY_DONE;
	}

	complete(&common->probe_completion);
	return NOTIFY_OK;
}

void pseudo_gpio_init(struct pseudo_gpio_common *common)
{
	memset(common, 0, sizeof(*common));
	init_completion(&common->probe_completion);
	common->bus_notifier.notifier_call = pseudo_gpio_notifier_call;
}
EXPORT_SYMBOL_GPL(pseudo_gpio_init);

int pseudo_gpio_register(struct pseudo_gpio_common *common,
			 struct platform_device_info *pdevinfo)
{
	struct platform_device *pdev;
	char *name;

	name = kasprintf(GFP_KERNEL, "%s.%u", pdevinfo->name, pdevinfo->id);
	if (!name)
		return -ENOMEM;

	common->driver_bound = false;
	common->name = name;
	reinit_completion(&common->probe_completion);
	bus_register_notifier(&platform_bus_type, &common->bus_notifier);

	pdev = platform_device_register_full(pdevinfo);
	if (IS_ERR(pdev)) {
		bus_unregister_notifier(&platform_bus_type, &common->bus_notifier);
		kfree(common->name);
		return PTR_ERR(pdev);
	}

	wait_for_completion(&common->probe_completion);
	bus_unregister_notifier(&platform_bus_type, &common->bus_notifier);

	if (!common->driver_bound) {
		platform_device_unregister(pdev);
		kfree(common->name);
		return -ENXIO;
	}

	common->pdev = pdev;
	return 0;
}
EXPORT_SYMBOL_GPL(pseudo_gpio_register);

void pseudo_gpio_unregister(struct pseudo_gpio_common *common)
{
	platform_device_unregister(common->pdev);
	kfree(common->name);
	common->pdev = NULL;
}
EXPORT_SYMBOL_GPL(pseudo_gpio_unregister);
