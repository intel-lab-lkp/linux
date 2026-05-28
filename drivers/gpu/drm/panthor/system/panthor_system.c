// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include "panthor_device_io.h"

#define AM_SYS_IRQ_RAWSTAT			0x40
#define AM_SYS_IRQ_CLEAR			0x4C
#define AM_SYS_IRQ_MASK				0x58
#define AM_SYS_IRQ_STATUS			0x70
#define   AM_SYS_IRQ_RESET_COMPLETED		BIT(1)
#define   AM_SYS_IRQ_INVALID_COMMAND		BIT(2)
#define   AM_SYS_IRQ_INVALID_ACCESS_MASK	GENMASK(31, 16)
#define   AM_SYS_IRQ_INVALID_ACCESS(x)		FIELD_GET(AM_IRQ_INVALID_ACCESS_MASK, x)

#define AM_SYS_STATUS				0xC0
#define AM_SYS_COMMAND				0xC4
#define   AM_SYS_CMD_INDEX(x)			FIELD_PREP(GENMASK(11, 8), x)
#define   AM_SYS_CMD_SOFT_RESET			0x10
#define   AM_SYS_CMD_HARD_RESET			0x11
#define   AM_SYS_CMD_AW_RESET			0x12

#define AM_SYS_RESET_SLEEP_US			10
#define AM_RESET_TIMEOUT_US			(20 * USEC_PER_MSEC)

/**
 * struct panthor_system - System device
 */
struct panthor_system {
	/** @dev: Device pointer */
	struct device *dev;

	/** @iomem: CPU mapping of the AM_SYSTEM IOMEM region */
	void __iomem *iomem;

	/** @regulator: Pointer to device regulator */
	struct regulator *regulator;

	/** @clk: Pointer to device clk */
	struct clk *clk;
};

static int panthor_system_regulator_init(struct panthor_system *sdev)
{
	struct device_node *np = sdev->dev->of_node;
	struct regulator *regulator = NULL;
	const char *regulator_name = NULL;
	int ret, cnt;

	cnt = of_property_count_strings(np, "clock-names");
	/* No clock-names defined. Exit */
	if (!cnt || (cnt == -EINVAL))
		return 0;
	else if (cnt < 0)
		return dev_err_probe(sdev->dev, cnt,
				     "Failed to count clock-names");

	ret = of_property_read_string_index(np, "clock-names", 0,
					    &regulator_name);
	if (ret)
		return dev_err_probe(sdev->dev, ret,
				     "Failed to get string from clock-names");

	regulator = devm_regulator_get_optional(sdev->dev, regulator_name);
	if (IS_ERR(regulator)) {
		ret = PTR_ERR(regulator);

		/* Regulator not found. Exit */
		if (ret == -ENODEV)
			return 0;

		return dev_err_probe(sdev->dev, ret, "Failed to get %s-supply",
				     regulator_name);
	}

	sdev->regulator = regulator;

	return 0;
}

static int panthor_system_regulator_resume(struct panthor_system *sdev)
{
	int ret;

	if (!sdev->regulator)
		return 0;

	ret = regulator_enable(sdev->regulator);
	if (ret) {
		dev_err(sdev->dev, "Failed to enable regulator");
		return ret;
	}

	return 0;
}

static int panthor_system_regulator_suspend(struct panthor_system *sdev)
{
	int ret;

	if (!sdev->regulator)
		return 0;

	ret = regulator_disable(sdev->regulator);
	if (ret) {
		dev_err(sdev->dev, "Failed to disable regulator");
		return ret;
	}

	return 0;
}

static int panthor_system_clk_init(struct panthor_system *sdev)
{
	struct clk *clk = NULL;

	clk = devm_clk_get(sdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(sdev->dev, PTR_ERR(clk),
				     "Failed to get clk");

	sdev->clk = clk;

	return 0;
}

static int panthor_system_clk_resume(struct panthor_system *sdev)
{
	int ret;

	ret = clk_prepare_enable(sdev->clk);
	if (ret) {
		dev_err(sdev->dev, "Failed to enable clk: %d", ret);
		return ret;
	}

	return 0;
}

static void panthor_system_clk_suspend(struct panthor_system *sdev)
{
	clk_disable_unprepare(sdev->clk);
}

static int __panthor_system_reset(struct panthor_system *sdev, u32 cmd)
{
	u64 val;

	gpu_write(sdev->iomem, AM_SYS_COMMAND, cmd);

	return read_poll_timeout(gpu_read64, val,
				 (val & AM_SYS_IRQ_RESET_COMPLETED),
				 AM_SYS_RESET_SLEEP_US, AM_RESET_TIMEOUT_US, false,
				 sdev->iomem, AM_SYS_IRQ_RAWSTAT);
}

static int panthor_system_reset(struct panthor_system *sdev)
{
	int ret;

	ret = __panthor_system_reset(sdev, AM_SYS_CMD_SOFT_RESET);
	if (ret) {
		dev_err(sdev->dev, "SOFT_RESET failed, attempting HARD_RESET");

		ret = __panthor_system_reset(sdev, AM_SYS_CMD_HARD_RESET);
		if (ret) {
			dev_err(sdev->dev, "HARD_RESET failed");
			return -EIO;
		}
	}

	gpu_write64(sdev->iomem, AM_SYS_IRQ_CLEAR, AM_SYS_IRQ_RESET_COMPLETED);

	return 0;
}

static int panthor_system_suspend(struct device *dev)
{
	struct panthor_system *sdev = dev_get_drvdata(dev);

	panthor_system_clk_suspend(sdev);
	return panthor_system_regulator_suspend(sdev);
}

static int panthor_system_resume(struct device *dev)
{
	struct panthor_system *sdev = dev_get_drvdata(dev);
	int ret;

	ret = panthor_system_regulator_resume(sdev);
	if (ret)
		return ret;

	ret = panthor_system_clk_resume(sdev);
	if (ret)
		goto err_regulator_suspend;

	return 0;

err_regulator_suspend:
	panthor_system_regulator_suspend(sdev);

	return ret;
}

static int panthor_system_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct panthor_system *sdev;
	void __iomem *iomem;
	int ret;

	sdev = devm_kzalloc(dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(iomem)) {
		dev_err(dev, "Failed to ioremap");
		return PTR_ERR(iomem);
	}

	sdev->dev = dev;
	sdev->iomem = iomem;

	ret = panthor_system_regulator_init(sdev);
	if (ret)
		return ret;

	ret = panthor_system_clk_init(sdev);
	if (ret)
		return ret;

	dev_set_drvdata(dev, sdev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable runtime PM");

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to resume device");

	ret = panthor_system_reset(sdev);
	if (ret) {
		dev_err(dev, "Failed to reset GPU");
		goto err_exit;
	}

	/* TODO: IRQs */

err_exit:
	pm_runtime_put_sync_suspend(dev);

	return ret;
}

static void panthor_system_remove(struct platform_device *pdev)
{
}

static const struct dev_pm_ops panthor_system_pm_ops = {
	.suspend = pm_runtime_force_suspend,
	.resume = pm_runtime_force_resume,
	.runtime_suspend = panthor_system_suspend,
	.runtime_resume = panthor_system_resume,
};

static const struct of_device_id panthor_system_dt_match[] = {
	{ .compatible = "arm,mali-gen5-am-system" },
	{}
};

static struct platform_driver panthor_system_driver = {
	.probe = panthor_system_probe,
	.remove = panthor_system_remove,
	.driver = {
		.name = "panthor_system",
		.pm = pm_ptr(&panthor_system_pm_ops),
		.of_match_table = panthor_system_dt_match,
	},
};

module_platform_driver(panthor_system_driver);

MODULE_AUTHOR("ARM Ltd.");
MODULE_DESCRIPTION("GPU system driver for AM GPUs");
MODULE_LICENSE("Dual MIT/GPL");
