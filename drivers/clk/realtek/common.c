// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include "common.h"

int rtk_clk_probe(struct platform_device *pdev, const struct rtk_clk_desc *desc)
{
	int i, ret;
	struct regmap *regmap;
	struct device *dev = &pdev->dev;
	struct rtk_reset_initdata reset_initdata = {0};

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

	if (!desc->num_reset_banks)
		return 0;

	if (!desc->reset_banks)
		return dev_err_probe(dev, -EINVAL,
				     "Missing reset banks data though num_reset_banks is %zu\n",
				     desc->num_reset_banks);

	reset_initdata.regmap = regmap;
	reset_initdata.num_banks = desc->num_reset_banks;
	reset_initdata.banks = desc->reset_banks;

	return rtk_reset_controller_add(dev, &reset_initdata);
}
EXPORT_SYMBOL_GPL(rtk_clk_probe);

MODULE_DESCRIPTION("Realtek clock infrastructure");
MODULE_LICENSE("GPL");
