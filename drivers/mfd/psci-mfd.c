// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

static struct fwnode_handle *psci_reboot_mode_get_child_fwnode(struct device *parent)
{
	struct fwnode_handle *fwnode;

	fwnode = fwnode_get_named_child_node(dev_fwnode(parent), "reboot-mode");
	if (!fwnode_device_is_available(fwnode)) {
		fwnode_handle_put(fwnode);
		fwnode = NULL;
	}

	return fwnode;
}

static const struct mfd_cell psci_cells[] = {
	{
		.name = "psci-cpuidle-domain",
	},
	{
		.name = "psci-reboot-mode",
		.get_child_fwnode = psci_reboot_mode_get_child_fwnode,
	},
};

static int psci_mfd_probe(struct platform_device *pdev)
{
	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO, psci_cells,
				   ARRAY_SIZE(psci_cells), NULL, 0, NULL);
}

static const struct of_device_id psci_mfd_of_match[] = {
	{ .compatible = "arm,psci-1.0" },
	{ }
};

static struct platform_driver psci_mfd_driver = {
	.probe = psci_mfd_probe,
	.driver = {
		.name = "psci-mfd",
		.of_match_table = psci_mfd_of_match,
	},
};

static int __init psci_mfd_init(void)
{
	return platform_driver_register(&psci_mfd_driver);
}

core_initcall(psci_mfd_init);

MODULE_LICENSE("GPL");
