// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#define PSCI_REBOOT_MODE_CELL_IDX 1

static const struct mfd_cell psci_cells[] = {
	{
		.name = "psci-cpuidle-domain",
	},
	{
		.name = "psci-reboot-mode",
	},
};

static int psci_mfd_probe(struct platform_device *pdev)
{
	struct mfd_cell cells[ARRAY_SIZE(psci_cells)];
	struct device_node *np = NULL;
	int ret;

	memcpy(cells, psci_cells, sizeof(cells));

	if (pdev->dev.of_node)
		np = of_get_child_by_name(pdev->dev.of_node, "reboot-mode");
	cells[PSCI_REBOOT_MODE_CELL_IDX].fwnode = of_fwnode_handle(np);

	ret = devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO, cells,
				   ARRAY_SIZE(cells), NULL, 0, NULL);
	of_node_put(np);

	return ret;
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
