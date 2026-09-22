// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/of_address.h>
#include <linux/of.h>

#include "panthor_arbitration.h"
#include "panthor_arbitration_sched.h"
#include "panthor_partition_control.h"
#include "panthor_resource_group.h"

#define PANTHOR_PM_AUTOSUSPEND_DELAY_MS 100

#define DT_ASSIGNED_AW				"arm,access-window-mask"
#define DT_ID					"arm,id"

static int to_assigned_sched_idx(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = -1;

	if (aw_id >= AM_ARB_MAX_AW_COUNT) {
		dev_err(adev->dev, "Invalid AW ID (%u)", aw_id);
		return -EINVAL;
	}

	idx = READ_ONCE(adev->aw_sched_map[aw_id]);
	if (idx < 0 || idx >= AM_ARB_MAX_PC_COUNT || !adev->sched[idx]) {
		dev_warn(adev->dev, "AW%u not assigned to any sched", aw_id);
		return -ENOENT;
	}

	return idx;
};

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

static u32 arbitration_get_sched_aw_mask(struct panthor_arbitration *adev,
					 u8 sched_id)
{
	u32 aw_mask = 0;

	lockdep_assert_held(&adev->assignment_lock);

	for (int i = 0; i < AM_ARB_MAX_AW_COUNT; i++) {
		if (adev->aw_sched_map[i] == sched_id)
			aw_mask |= BIT(i);
	}

	return aw_mask;
}

static int arbitration_dt_parse_assigned_aw(struct panthor_arbitration *adev)
{
	struct panthor_arbitration_dt_info *dt_info = &adev->dt_info;
	struct device *dev = adev->dev;
	u32 seen_mask = 0;
	u8 seen_partitions = 0;
	int ret;

	device_for_each_named_child_node_scoped(dev, child, "partition") {
		struct panthor_arbitration_resource res;

		if (panthor_arbitration_parse_resource(dev, child, &res))
			continue;

		if (res.id >= AM_ARB_MAX_PC_COUNT) {
			dev_warn(dev,
				 "partition@%llx: id (%u) >= %u not supported",
				 res.res.start, res.id, AM_ARB_MAX_PC_COUNT);
			continue;
		}

		if (BIT(res.id) & seen_partitions) {
			dev_warn(dev, "partition@%llx: id (%u) already present",
				 res.res.start, res.id);
			continue;
		}

		seen_partitions |= BIT(res.id);

		if (!fwnode_property_present(child, DT_ASSIGNED_AW))
			continue;

		ret = fwnode_property_read_u32(child, DT_ASSIGNED_AW,
					       &dt_info->default_aw_masks[res.id]);
		if (ret)
			continue;
	}

	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		u32 aw_mask = dt_info->default_aw_masks[i];

		if (aw_mask & ~GENMASK(AM_ARB_MAX_AW_COUNT - 1, 0))
			return dev_err_probe(dev, -EINVAL, "%s[%d] entry exceeds max AW count",
					     DT_ASSIGNED_AW, i);

		if (aw_mask & seen_mask)
			return dev_err_probe(dev, -EINVAL, "%s entries overlap",
					     DT_ASSIGNED_AW);

		seen_mask |= aw_mask;
	}

	return 0;
}

static int arbitration_dt_parse_properties(struct panthor_arbitration *adev)
{
	return arbitration_dt_parse_assigned_aw(adev);
}

static int arbitration_sched_try_start(struct panthor_arbitration *adev, u8 sched_id)
{
	struct panthor_arbitration_sched *sched;
	u32 aw_mask;

	if (!adev->sched[sched_id])
		return -EINVAL;

	sched = adev->sched[sched_id];

	scoped_guard(mutex, &adev->assignment_lock)
		aw_mask = arbitration_get_sched_aw_mask(adev, sched_id);

	if (aw_mask)
		panthor_arbitration_sched_disable_clear(sched, ARB_SCHED_DISABLE_RESOURCE);
	else
		panthor_arbitration_sched_disable_set(sched, ARB_SCHED_DISABLE_RESOURCE);

	return panthor_arbitration_sched_start(sched);
}

static int arbitration_resource_init(struct panthor_arbitration *adev)
{
	struct panthor_arbitration_dt_info *dt_info = &adev->dt_info;

	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		unsigned long pc_aw_mask = dt_info->default_aw_masks[i];
		u8 aw_id;
		int ret;

		if (!adev->pc[i] || !adev->sched[i])
			continue;

		scoped_guard(mutex, &adev->assignment_lock) {
			for_each_set_bit(aw_id, &pc_aw_mask, AM_ARB_MAX_AW_COUNT)
				WRITE_ONCE(adev->aw_sched_map[aw_id], i);
		}

		ret = arbitration_sched_try_start(adev, i);
		if (ret)
			return ret;
	}
	return 0;
}

/* AW to Arbiter events */
int panthor_arbitration_on_request(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_arbitration_sched_on_request(adev->sched[idx], aw_id);
}

int panthor_arbitration_on_idle(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_arbitration_sched_on_idle(adev->sched[idx], aw_id);
}

int panthor_arbitration_on_stopped(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_arbitration_sched_on_stopped(adev->sched[idx], aw_id);
}

/* Arbiter to AW events */
int panthor_arbitration_on_grant(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_partition_control_open_window(adev->pc[idx], aw_id);
}

int panthor_arbitration_on_stop(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_partition_control_yield_now(adev->pc[idx]);
}

int panthor_arbitration_on_close(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_partition_control_close_window(adev->pc[idx]);
}

int panthor_arbitration_notify_yield_idle(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	return panthor_partition_control_yield_idle(adev->pc[idx]);
}

int panthor_arbitration_notify_err(struct panthor_arbitration *adev, u8 aw_id)
{
	int idx = to_assigned_sched_idx(adev, aw_id);

	if (idx < 0)
		return idx;

	panthor_arbitration_sched_reset(adev->sched[idx], aw_id);

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

	mutex_init(&adev->assignment_lock);
	for (int i = 0; i < AM_ARB_MAX_AW_COUNT; i++)
		adev->aw_sched_map[i] = -1;

	dev_set_drvdata(dev, adev);

	ret = arbitration_dt_parse_properties(adev);
	if (ret)
		return ret;

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

	ret = arbitration_resource_init(adev);
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
