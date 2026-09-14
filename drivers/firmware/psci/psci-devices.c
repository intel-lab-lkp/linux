// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/auxiliary_bus.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static int arm_psci_probe(struct platform_device *pdev)
{
	struct auxiliary_device *auxdev;

	auxdev = __devm_auxiliary_device_create(&pdev->dev, "arm-psci",
						"psci-cpuidle-domain", NULL, 0);
	if (!auxdev)
		return -ENOMEM;

	return 0;
}

static const struct of_device_id arm_psci_of_match[] = {
	{ .compatible = "arm,psci-1.0" },
	{ }
};

static struct platform_driver arm_psci_driver = {
	.probe = arm_psci_probe,
	.driver = {
		.name = "arm-psci",
		.of_match_table = arm_psci_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init arm_psci_init(void)
{
	return platform_driver_register(&arm_psci_driver);
}
core_initcall(arm_psci_init);
