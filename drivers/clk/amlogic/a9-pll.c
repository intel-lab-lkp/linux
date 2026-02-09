// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-pll.h"

static const struct aml_pll_data a9_mclk_pll_data = {
	.range = {
		.min = 1400000000,
		.max = 2800000000,
	},
	.od_max = 4,
};

static const struct aml_pll_data a9_gp_pll_data = {
	.range = {
		.min = 1400000000,
		.max = 2800000000,
	},
	.frac_max = 131072, /* 2^17 */
	.od_max = 4,
	.flags = AML_PLL_M_EN0P5,
};

static const struct aml_pll_data a9_hifi_pll_data = {
	.range = {
		.min = 1400000000,
		.max = 2800000000,
	},
	/*
	 * NOTE: The frac_max value of hifi_pll is set to 100000 so that the
	 * output frequency step can be expressed as an integer value. For
	 * example, with a 24 MHz input clock, the resulting frequency step is
	 * 24 MHz / 100000 = 240 Hz.
	 *
	 * This design avoids the need for floating-point arithmetic in
	 * frequency calculations, which helps prevent precision loss in
	 * scenarios with strict frequency accuracy requirements, such as audio
	 * and video applications.
	 */
	.frac_max = 100000,
	.od_max = 4,
	.flags = AML_PLL_M_EN0P5,
};

static int of_aml_clk_pll_init_register(struct device *dev, struct aml_clk *pll)
{
	struct device_node *np = dev_of_node(dev);
	struct clk_init_data init;
	struct clk_parent_data pdata;
	u8 pnum;
	int ret;

	init.name = of_aml_clk_get_name_index(np, 0);
	if (!init.name)
		return -EINVAL;

	ret = of_aml_clk_get_parent_data(dev, NULL, 0, 0, &pdata, &pnum);
	if (ret)
		return ret;

	init.ops = &aml_pll_ops;
	init.num_parents = pnum;
	init.parent_data = &pdata;

	pll->hw.init = &init;
	ret = of_aml_clk_register(dev, &pll->hw, 0);
	if (ret)
		return ret;

	return 0;
}

static int of_aml_clk_pll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	struct aml_clk *pll;
	struct aml_pll_data *pll_data_tmp;
	int ret;

	pll_data_tmp = (void *)of_device_get_match_data(dev);
	if (!pll_data_tmp)
		return -EFAULT;

	pll = devm_kmalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;

	pll->data = devm_kmemdup(dev, pll_data_tmp, sizeof(*pll_data_tmp),
				 GFP_KERNEL);
	if (!pll->data)
		return -ENOMEM;

	regmap = aml_clk_regmap_init(pdev);
	if (IS_ERR_OR_NULL(regmap))
		return -EIO;

	pll->map = regmap;
	pll->type = AML_CLKTYPE_PLL;
	ret = of_aml_clk_regs_init(dev);
	if (ret)
		return ret;

	ret = of_aml_clk_pll_init_register(dev, pll);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &pll->hw);
}

static const struct of_device_id of_aml_clk_pll_match_table[] = {
	{
		.compatible = "amlogic,a9-int-pll",
		.data = &a9_mclk_pll_data,
	},
	{
		.compatible = "amlogic,a9-frac-pll",
		.data = &a9_gp_pll_data,
	},
	{
		.compatible = "amlogic,a9-frac-step-pll",
		.data = &a9_hifi_pll_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, of_aml_clk_pll_match_table);

static struct platform_driver of_aml_clk_pll_driver = {
	.probe		= of_aml_clk_pll_probe,
	.driver		= {
		.name	= "aml-pll",
		.of_match_table = of_aml_clk_pll_match_table,
	},
};
module_platform_driver(of_aml_clk_pll_driver);

MODULE_DESCRIPTION("Amlogic A9 PLL Controllers Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
