// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Marek Vasut
 */

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>

struct stm32mp2_syscfg_data {
	bool has_clk;
};

static int stm32mp2_syscfg_probe(struct platform_device *pdev)
{
	const struct stm32mp2_syscfg_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	ret = devm_of_platform_populate(dev);
	if (ret)
		return ret;

	data = device_get_match_data(dev);
	if (data && data->has_clk &&
	    !devm_auxiliary_device_create(dev, "clk", NULL))
		return dev_err_probe(dev, -ENODEV,
				     "failed to register the clock device\n");

	return 0;
}

static const struct stm32mp2_syscfg_data stm32mp25_syscfg_data = {
	.has_clk = true,
};

static const struct of_device_id stm32mp2_syscfg_ids[] = {
	{ .compatible = "st,stm32mp23-syscfg" },
	{ .compatible = "st,stm32mp25-syscfg", .data = &stm32mp25_syscfg_data },
	{ }
};
MODULE_DEVICE_TABLE(of, stm32mp2_syscfg_ids);

static struct platform_driver stm32mp2_syscfg_driver = {
	.driver = {
		.name	= "stm32mp2_syscfg",
		.of_match_table = stm32mp2_syscfg_ids,
	},
	.probe = stm32mp2_syscfg_probe,
};
module_platform_driver(stm32mp2_syscfg_driver);

MODULE_AUTHOR("Marek Vasut <marex@nabladev.com>");
MODULE_DESCRIPTION("ST STM32MP2 SYSCFG driver");
MODULE_LICENSE("GPL");
