// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-rtk-common.h"

static int rtk_reset_controller_register(struct device *dev, const char *aux_name,
					 struct regmap *map)
{
	struct auxiliary_device *adev;

	if (!of_property_present(dev->of_node, "#reset-cells"))
		return 0;

	if (!aux_name) {
		dev_err(dev, "DTS requires reset controller, but aux_name is missing\n");
		return -EINVAL;
	}

	adev = devm_auxiliary_device_create(dev, aux_name, (void *)map);
	if (!adev)
		return -ENOMEM;

	return 0;
}

int rtk_clk_probe(struct platform_device *pdev, const struct rtk_clk_desc *desc)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct clk_hw *hw;
	int i, ret;

	regmap = device_node_to_regmap(dev->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to get regmap\n");

	for (i = 0; i < desc->num_clks; i++)
		desc->clks[i]->regmap = regmap;

	for (i = 0; i < desc->clk_data->num; i++) {
		hw = desc->clk_data->hws[i];
		if (!hw)
			continue;

		ret = devm_clk_hw_register(dev, hw);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register hw of clk%d\n", i);
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					  desc->clk_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock provider\n");

	platform_set_drvdata(pdev, (void *)desc);

	return rtk_reset_controller_register(dev, desc->aux_name, regmap);
}
EXPORT_SYMBOL_NS_GPL(rtk_clk_probe, "CLK_REALTEK");

void rtk_clk_remove(struct platform_device *pdev)
{
	const struct rtk_clk_desc *desc = platform_get_drvdata(pdev);

	if (!desc)
		return;

	for (int i = 0; i < desc->num_clks; i++)
		desc->clks[i]->regmap = NULL;
}
EXPORT_SYMBOL_NS_GPL(rtk_clk_remove, "CLK_REALTEK");

MODULE_DESCRIPTION("Realtek clock infrastructure");
MODULE_LICENSE("GPL");
