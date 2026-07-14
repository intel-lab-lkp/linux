// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

static const struct mfd_cell psci_cells[] = {
	{
		.name = "psci-cpuidle-domain",
	},
};

static const struct mfd_cell psci_reboot_mode_cell[] = {
	{
		.name = "psci-reboot-mode",
		.named_fwnode = "reboot-mode",
	},
};

static int psci_mfd_probe(struct platform_device *pdev)
{
	struct fwnode_handle *fwnode;
	int ret;

	ret = devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO, psci_cells,
				   ARRAY_SIZE(psci_cells), NULL, 0, NULL);
	if (ret)
		return ret;

	fwnode = device_get_named_child_node(&pdev->dev, "reboot-mode");
	if (!fwnode)
		return 0;

	fwnode_handle_put(fwnode);

	ret = devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
				   psci_reboot_mode_cell,
				   ARRAY_SIZE(psci_reboot_mode_cell),
				   NULL, 0, NULL);
	if (ret)
		dev_warn(&pdev->dev, "reboot-mode child cell failed to add: %d\n", ret);

	return 0;
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
