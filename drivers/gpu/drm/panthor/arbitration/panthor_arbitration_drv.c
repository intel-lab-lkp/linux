// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>

#include "panthor_arbitration.h"
#include "panthor_partition_control.h"
#include "panthor_resource_group.h"

#define PANTHOR_PM_AUTOSUSPEND_DELAY_MS 100

static int panthor_arbitration_runtime_suspend(struct device *dev)
{
	struct panthor_arbitration *adev = dev_get_drvdata(dev);
	int ret = 0;

	ret = panthor_partition_control_suspend(adev);
	if (ret)
		return ret;

	ret = panthor_resource_group_suspend(adev);
	if (ret)
		return ret;

	return 0;
}

static int panthor_arbitration_runtime_resume(struct device *dev)
{
	struct panthor_arbitration *adev = dev_get_drvdata(dev);
	int ret = 0;

	ret = panthor_resource_group_resume(adev);
	if (ret)
		return ret;

	ret = panthor_partition_control_resume(adev);
	if (ret)
		return ret;

	return 0;
}

static int panthor_arbitration_probe(struct platform_device *pdev)
{
	struct panthor_arbitration *adev;
	struct device *dev = &pdev->dev;
	int ret;

	if (!pdev)
		return -EINVAL;

	adev = devm_kzalloc(dev, sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->dev = dev;

	dev_set_drvdata(dev, adev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = panthor_partition_control_init(adev);
	if (ret)
		goto err_out;

	ret = panthor_resource_group_init(adev);
	if (ret)
		goto err_term_partition;

	pm_runtime_set_autosuspend_delay(dev, PANTHOR_PM_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	pm_runtime_put_autosuspend(dev);

	return 0;

err_term_partition:
	panthor_partition_control_term(adev);

err_out:
	pm_runtime_put_noidle(dev);
	return ret;
}

static void panthor_arbitration_remove(struct platform_device *pdev)
{
	struct panthor_arbitration *adev = platform_get_drvdata(pdev);
	int ret;

	if (!adev)
		return;

	ret = pm_runtime_resume_and_get(adev->dev);
	if (ret < 0)
		goto out_suspended;

	panthor_resource_group_term(adev);
	panthor_partition_control_term(adev);

	pm_runtime_put_noidle(adev->dev);

out_suspended:
	pm_runtime_set_suspended(adev->dev);
}

static const struct dev_pm_ops panthor_arbitration_pm_ops = {
	.resume = pm_runtime_force_resume,
	.suspend = pm_runtime_force_suspend,
	.runtime_resume = panthor_arbitration_runtime_resume,
	.runtime_suspend = panthor_arbitration_runtime_suspend,
};

static const struct of_device_id panthor_arbitration_of_match[] = {
	{ .compatible = "arm,mali-gen5-am-arbitration" },
	{ /* Sentinel */ },
};

static struct platform_driver panthor_arbitration_driver = {
	.probe = panthor_arbitration_probe,
	.remove = panthor_arbitration_remove,
	.driver = {
		.name = "panthor-arbitration",
		.of_match_table = panthor_arbitration_of_match,
		.pm = pm_ptr(&panthor_arbitration_pm_ops),
	},
};

module_platform_driver(panthor_arbitration_driver);

MODULE_AUTHOR("ARM Ltd.");
MODULE_DESCRIPTION("Panthor HW-assisted virtualization Driver");
MODULE_LICENSE("Dual MIT/GPL");
