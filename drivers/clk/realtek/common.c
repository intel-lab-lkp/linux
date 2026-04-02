// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "common.h"

static int rtk_reset_controller_register(struct device *dev, const char *aux_name)
{
	struct auxiliary_device *adev;

	if (!of_property_present(dev->of_node, "#reset-cells"))
		return 0;

	adev = devm_auxiliary_device_create(dev, aux_name, NULL);

	if (IS_ERR(adev))
		return PTR_ERR(adev);
	return 0;
}

int rtk_clk_probe(struct platform_device *pdev, const struct rtk_clk_desc *desc,
		  const char *aux_name)
{
	int i, ret;
	struct regmap *regmap;
	struct device *dev = &pdev->dev;

	regmap = device_node_to_regmap(pdev->dev.of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "failed to get regmap\n");

	for (i = 0; i < desc->num_clks; i++)
		desc->clks[i]->regmap = regmap;

	for (i = 0; i < desc->clk_data->num; i++) {
		struct clk_hw *hw = desc->clk_data->hws[i];

		if (!hw)
			continue;

		ret = devm_clk_hw_register(dev, hw);

		if (ret) {
			dev_warn(dev, "failed to register hw of clk%d: %d\n", i,
				 ret);
			desc->clk_data->hws[i] = NULL;
		}
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					  desc->clk_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clock provider\n");

	return rtk_reset_controller_register(dev, aux_name);
}
EXPORT_SYMBOL_NS_GPL(rtk_clk_probe, "REALTEK_CLK");

MODULE_DESCRIPTION("Realtek clock infrastructure");
MODULE_LICENSE("GPL");
