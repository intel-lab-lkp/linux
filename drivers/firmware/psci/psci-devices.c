// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/slab.h>

static void arm_psci_auxiliary_device_release(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);

	of_node_put(dev->of_node);
	kfree(auxdev);
}

static struct auxiliary_device *
arm_psci_auxiliary_device_create(struct device *dev, const char *devname,
				 struct device_node *np)
{
	struct auxiliary_device *auxdev;
	int ret;

	auxdev = kzalloc_obj(*auxdev);
	if (!auxdev)
		return NULL;

	auxdev->id = 0;
	auxdev->name = devname;
	auxdev->dev.parent = dev;
	auxdev->dev.release = arm_psci_auxiliary_device_release;

	if (np)
		device_set_node(&auxdev->dev, of_fwnode_handle(of_node_get(np)));

	ret = auxiliary_device_init(auxdev);
	if (ret) {
		of_node_put(auxdev->dev.of_node);
		kfree(auxdev);
		return NULL;
	}

	ret = __auxiliary_device_add(auxdev, "arm-psci");
	if (ret) {
		auxiliary_device_uninit(auxdev);
		return NULL;
	}

	ret = devm_add_action_or_reset(dev, auxiliary_device_destroy, auxdev);
	if (ret)
		return NULL;

	return auxdev;
}

static int arm_psci_probe(struct platform_device *pdev)
{
	struct auxiliary_device *auxdev;
#ifdef CONFIG_PSCI_REBOOT_MODE
	struct device_node *reboot_mode_np = NULL;
#endif

	auxdev = arm_psci_auxiliary_device_create(&pdev->dev,
						  "psci-cpuidle-domain",
						  pdev->dev.of_node);
	if (!auxdev)
		return -ENOMEM;

#ifdef CONFIG_PSCI_REBOOT_MODE
	if (psci_has_system_reset2_support())
		reboot_mode_np = of_get_child_by_name(pdev->dev.of_node,
						      "reboot-mode");

	auxdev = arm_psci_auxiliary_device_create(&pdev->dev,
						  "psci-reboot-mode",
						  reboot_mode_np);
	of_node_put(reboot_mode_np);
	if (!auxdev)
		dev_warn(&pdev->dev, "failed to create PSCI reboot mode device\n");
#endif

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
