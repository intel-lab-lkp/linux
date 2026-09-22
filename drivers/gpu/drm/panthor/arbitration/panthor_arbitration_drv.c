// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>
#include <linux/of_address.h>
#include <linux/of.h>

#include "panthor_arbitration.h"
#include "panthor_arbitration_sched.h"
#include "panthor_partition_control.h"
#include "panthor_resource_group.h"

#define PANTHOR_PM_AUTOSUSPEND_DELAY_MS 100

#define DT_ID					"arm,id"

int panthor_arbitration_parse_resource(struct device *dev,
				       const struct fwnode_handle *child,
				       struct panthor_arbitration_resource *res)
{
	const char *name;
	int ret;

	if (!child || !res)
		return -EINVAL;

	name = fwnode_get_name(child);

	if (of_address_to_resource(to_of_node(child), 0, &res->res)) {
		dev_warn(dev, "Missing 'reg' property in %s", name);
		return -ENODEV;
	}

	if (!fwnode_property_present(child, DT_ID)) {
		dev_warn(dev, "Missing '%s' property in %s@%llx", DT_ID, name,
			 res->res.start);
		return -ENODEV;
	}

	ret = fwnode_property_read_u32(child, DT_ID, &res->id);
	if (ret) {
		dev_warn(dev, "Malformed '%s' property in %s@%llx", DT_ID, name,
			 res->res.start);
		return -ENODEV;
	}

	return 0;
}

static int panthor_arbitration_runtime_suspend(struct device *dev)
{
	struct panthor_arbitration *adev = dev_get_drvdata(dev);
	int ret = 0;

	if (adev->tearing_down)
		return 0;

	ret = panthor_arbitration_sched_suspend(adev);
	if (ret)
		return ret;

	ret = panthor_resource_group_suspend(adev);
	if (ret)
		return ret;

	ret = panthor_partition_control_suspend(adev);
	if (ret)
		return ret;

	return 0;
}

static int panthor_arbitration_runtime_resume(struct device *dev)
{
	struct panthor_arbitration *adev = dev_get_drvdata(dev);
	int ret = 0;

	ret = panthor_partition_control_resume(adev);
	if (ret)
		return ret;

	ret = panthor_resource_group_resume(adev);
	if (ret)
		return ret;

	ret = panthor_arbitration_sched_resume(adev);
	if (ret)
		return ret;

	return 0;
}

/* Arbiter to AW events */
int panthor_arbitration_on_grant(struct panthor_arbitration *adev, u8 aw_id)
{
	return 0;
}

int panthor_arbitration_on_stop(struct panthor_arbitration *adev, u8 aw_id)
{
	return 0;
}

int panthor_arbitration_on_close(struct panthor_arbitration *adev, u8 aw_id)
{
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
		goto err_term_pc;

	ret = panthor_resource_group_init(adev);
	if (ret)
		goto err_term_rg;

	ret = panthor_arbitration_sched_init(adev);
	if (ret)
		goto err_term_sched;

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret)
		goto err_depopulate;

	pm_runtime_set_autosuspend_delay(dev, PANTHOR_PM_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);

	pm_runtime_put_autosuspend(dev);

	return 0;

err_depopulate:
	of_platform_depopulate(dev);
err_term_sched:
	panthor_arbitration_sched_term(adev);
err_term_rg:
	panthor_resource_group_cancel_work(adev);
	panthor_resource_group_term(adev);
err_term_pc:
	panthor_partition_control_term(adev);
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
		dev_warn(adev->dev, "failed to resume during remove: %d", ret);

	of_platform_depopulate(adev->dev);

	if (ret >= 0) {
		adev->tearing_down = true;

		panthor_arbitration_sched_term(adev);
		panthor_resource_group_cancel_work(adev);
		panthor_resource_group_term(adev);
		panthor_partition_control_term(adev);
		pm_runtime_put_sync_suspend(adev->dev);
	} else {
		panthor_arbitration_sched_cancel(adev);
		panthor_resource_group_cancel_work(adev);
		pm_runtime_set_suspended(adev->dev);
	}
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
