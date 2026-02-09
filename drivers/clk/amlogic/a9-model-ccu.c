// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-basic.h"
#include "clk-composite.h"
#include "clk-module.h"
#include "clk-noglitch.h"

/*
 * The standardized model clock control units of Amlogic includes:
 *   - composite-ccu
 *   - noglitch-ccu
 *   - sysbus-ccu
 */
#define MAX_AML_CLK_COMP_PARENTS		8

enum aml_clk_model_type {
	CLK_MODEL_COMPOSITE	= 1,
	CLK_MODEL_NOGLITCH	= 2,
	CLK_MODEL_SYSBUS	= 3,
};

struct aml_clk_model_data {
	enum aml_clk_model_type	type;
};

static int of_aml_clk_model_get_type(struct device *dev,
				     enum aml_clk_type *out_type)
{
	const struct aml_clk_model_data *data;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EFAULT;

	switch (data->type) {
	case CLK_MODEL_COMPOSITE:
		*out_type = AML_CLKTYPE_COMPOSITE;
		return 0;

	case CLK_MODEL_NOGLITCH:
		*out_type = AML_CLKTYPE_NOGLITCH;
		return 0;

	case CLK_MODEL_SYSBUS:
		*out_type = AML_CLKTYPE_GATE;
		return 0;

	default:
		return -EINVAL;
	}
}

/*
 * A diagram of the A9 composite-ccu is as follows:
 *         +----------------------------------+
 *         |                                  |
 *         |   |\                             |
 * clk0 ------>| |                            |
 * clk1 ------>| |                            |
 * clk2 ------>| |                            |
 * clk3 ------>| |     +-----+     +------+   |
 *         |   | |---->| div |---->| gate |------> clk out
 * clk4 ------>| |     +-----+     +------+   |
 * clk5 ------>| |                            |
 * clk6 ------>| |                            |
 * clk7 ------>| |                            |
 *         |   |/                             |
 *         |                                  |
 *         +----------------------------------+
 */
static int
of_aml_clk_model_composite_init_register(struct device *dev,
					 struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct aml_clk_composite_data *composite;
	u32 reg_val[3];
	unsigned int pcnt = of_clk_get_parent_count(np);
	struct clk_parent_data pdata[MAX_AML_CLK_COMP_PARENTS];
	struct clk_parent_data pdata_compb[MAX_AML_CLK_COMP_PARENTS];
	u8 pnum, pnum_compb;
	u32 *ptab, *ptab_compb;
	struct clk_init_data init = { 0 };
	int index;
	int ret, clkid, i;

	ret = of_aml_clk_get_parent_data(dev, hw_data->hws, 0, 7, pdata, &pnum);
	if (ret)
		return ret;

	ptab = of_aml_clk_get_parent_table(dev, 0, 7);
	if (IS_ERR(ptab))
		return PTR_ERR(ptab);

	/*
	 * If the number of "clocks" defined in DT is less than or equal
	 * to MAX_AML_CLK_COMP_PARENTS, composite-ccu_a and composite-ccu_b
	 * share the same parent clocks.
	 *
	 * If the number of "clocks" defined in the DT is greater than
	 * MAX_AML_CLK_COMP_PARENTS, composite-ccu_a and composite-ccu_b have
	 * different parent clocks, the parent clocks specified by
	 * "clocks" follow the rule below:
	 *   - for composite-ccu_a: clocks indices 0-7 in "clocks"
	 *   - for composite-ccu_b: clocks indices 8-15 in "clocks"
	 */
	if (pcnt > MAX_AML_CLK_COMP_PARENTS) { /* composite-ccu_b */
		ret = of_aml_clk_get_parent_data(dev, hw_data->hws, 8, 15,
						 pdata_compb, &pnum_compb);
		if (ret)
			return ret;

		ptab_compb = of_aml_clk_get_parent_table(dev, 8, 15);
		if (IS_ERR(ptab_compb))
			return PTR_ERR(ptab_compb);
	}

	init.ops = &aml_clk_composite_ops;
	for (clkid = 0; clkid < hw_data->num; clkid++) {
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		/*
		 * The set of "amlogic,reg-layout" attributes of composite_ccu
		 * contains three u32 data:
		 *   - reg_offset
		 *   - bit_offset
		 *   - div_width
		 */
		index = clkid * 3;
		for (i = 0; i < 3; i++) {
			ret = of_property_read_u32_index(np,
							 "amlogic,reg-layout",
							 index + i, &reg_val[i]);
			if (ret)
				return ret;
		}

		composite = devm_kzalloc(dev, sizeof(*composite), GFP_KERNEL);
		if (!composite)
			return -ENOMEM;

		composite->reg_offset = reg_val[0];
		composite->bit_offset = reg_val[1];
		composite->div_width = reg_val[2];

		/*
		 * The register bit allocation for composite-ccu_a and
		 * composite-ccu_b is as follows:
		 *   - composite-ccu_a: bit[15: 0]
		 *   - composite-ccu_b: bit[31: 16]
		 * A value of "composite->bit_offset == 16" indicates that this
		 * CCU corresponds to composite-ccu_b.
		 */
		if (pcnt > MAX_AML_CLK_COMP_PARENTS &&
		    composite->bit_offset == 16) { /* composite-ccu_b */
			init.num_parents = pnum_compb;
			init.parent_data = pdata_compb;

			if (ptab_compb)
				composite->table = ptab_compb;
		} else { /* composite-ccu_a */
			init.num_parents = pnum;
			init.parent_data = pdata;

			if (ptab)
				composite->table = ptab;
		}

		clk = to_aml_clk(hw_data->hws[clkid]);
		clk->data = composite;

		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * A diagram of the A9 noglitch-ccu is as follows:
 *         +---------------------------------------------+
 *         |   |\                                        |
 * clk0 ------>| |                                       |
 * clk1 ------>| |                                       |
 * clk2 ------>| |                                       |
 * clk3 ------>| |      +-----+      +------+      |\    |
 *         |   | |----->| div |----->| gate |----->| |   |
 * clk4 ------>| |      +-----+      +------+      | |   |
 * clk5 ------>| |                                 | |   |
 * clk6 ------>| |                                 | |   |
 * clk7 ------>| |                                 | |   |
 *         |   |/                                  | |   |
 *         |                                       | |-------> clk out
 *         |   |\                                  | |   |
 * clk0 ------>| |                                 | |   |
 * clk1 ------>| |                                 | |   |
 * clk2 ------>| |                                 | |   |
 * clk3 ------>| |      +-----+     +------+       | |   |
 *         |   | |----->| div |---->| gate |------>| |   |
 * clk4 ------>| |      +-----+     +------+       |/    |
 * clk5 ------>| |                                       |
 * clk6 ------>| |                                       |
 * clk7 ------>| |                                       |
 *         |   |/                                        |
 *         +---------------------------------------------+
 */
static int
of_aml_clk_model_noglitch_init_register(struct device *dev,
					struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct aml_clk_noglitch_data *noglitch;
	struct clk_parent_data pdata[MAX_AML_CLK_COMP_PARENTS];
	u8 pnum;
	u32 *ptab;
	struct clk_init_data init = { 0 };
	int ret, clkid;

	/* noglitch-ccu supports up to eight parent clocks. */
	ret = of_aml_clk_get_parent_data(dev, hw_data->hws, 0, 7, pdata, &pnum);
	if (ret)
		return ret;

	ptab = of_aml_clk_get_parent_table(dev, 0, 7);
	if (IS_ERR(ptab))
		return PTR_ERR(ptab);

	init.ops = &aml_clk_noglitch_ops;
	init.num_parents = pnum;
	init.parent_data = pdata;
	for (clkid = 0; clkid < hw_data->num; clkid++) {
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		hw_data->hws[clkid]->init = &init;

		noglitch = devm_kzalloc(dev, sizeof(*noglitch), GFP_KERNEL);
		if (!noglitch)
			return -ENOMEM;

		ret = of_property_read_u32_index(np, "amlogic,reg-layout",
						 clkid, &noglitch->reg_offset);
		if (ret)
			return ret;

		if (ptab)
			noglitch->table = ptab;

		clk = to_aml_clk(hw_data->hws[clkid]);
		clk->data = noglitch;

		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * A diagram of the A9 sysbus-ccu is as follows:
 *         +-------------------+
 *         |                   |
 *         |        +------+   |
 *         |  +---->| gate |----->clkout0
 *         |  |     +------+   |
 *         |  |                |
 *         |  |                |
 *         |  |                |
 *         |  |     +------+   |
 * clk in-----+---->| gate |----->clkout1
 *         |  |     +------+   |
 *         |  |        ...     |
 *         |  |                |
 *         |  |                |
 *         |  |     +------+   |
 *         |  +---->| gate |----->clkoutn
 *         |        +------+   |
 *         |                   |
 *         +-------------------+
 */
static int
of_aml_clk_model_sysbus_init_register(struct device *dev,
				      struct clk_hw_onecell_data *hw_data)
{
	struct device_node *np = dev_of_node(dev);
	struct aml_clk *clk;
	struct aml_clk_gate_data *gate;
	u32 reg_val[2];
	struct clk_parent_data pdata;
	u8 pnum;
	struct clk_init_data init = { 0 };
	int index;
	int ret, clkid, i;

	ret = of_aml_clk_get_parent_data(dev, hw_data->hws, 0, 0, &pdata, &pnum);
	if (ret)
		return ret;

	init.ops = &aml_clk_gate_ops;
	init.num_parents = pnum;
	init.parent_data = &pdata;
	for (clkid = 0; clkid < hw_data->num; clkid++) {
		init.name = of_aml_clk_get_name_index(np, clkid);
		if (!init.name)
			return -EINVAL;

		/*
		 * The set of "amlogic,reg-layout" attributes of sysbus_ccu
		 * contains three u32 data:
		 *   - reg_offset
		 *   - bit_idx
		 */
		index = clkid * 2;
		for (i = 0; i < 2; i++) {
			ret = of_property_read_u32_index(np,
							 "amlogic,reg-layout",
							 index + i, &reg_val[i]);
			if (ret)
				return ret;
		}

		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate)
			return -ENOMEM;

		gate->reg_offset = reg_val[0];
		gate->bit_idx = reg_val[1];

		clk = to_aml_clk(hw_data->hws[clkid]);
		clk->data = gate;

		hw_data->hws[clkid]->init = &init;
		ret = of_aml_clk_register(dev, hw_data->hws[clkid], clkid);
		if (ret)
			return ret;
	}

	return 0;
}

static int of_aml_clk_model_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct aml_clk_model_data *data;
	struct device_node *np = dev_of_node(dev);
	struct regmap *regmap;
	struct clk_hw_onecell_data *hw_data;
	struct aml_clk *clk;
	enum aml_clk_type clk_type;
	int clk_num;
	int ret, i;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EFAULT;

	clk_num = of_aml_clk_get_count(np);
	if (clk_num == 0)
		return -EINVAL;

	regmap = aml_clk_regmap_init(pdev);
	if (IS_ERR_OR_NULL(regmap))
		return -EIO;

	of_aml_clk_regs_init(dev);

	hw_data = devm_kzalloc(dev, struct_size(hw_data, hws, clk_num),
				GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	hw_data->num = clk_num;
	clk = devm_kcalloc(dev, clk_num, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;

	ret = of_aml_clk_model_get_type(dev, &clk_type);
	if (ret)
		return ret;

	for (i = 0; i < clk_num; i++) {
		clk[i].map = regmap;
		clk[i].type = clk_type;
		hw_data->hws[i] = &clk[i].hw;
	}

	if (data->type == CLK_MODEL_COMPOSITE)
		ret = of_aml_clk_model_composite_init_register(dev, hw_data);
	else if (data->type == CLK_MODEL_NOGLITCH)
		ret = of_aml_clk_model_noglitch_init_register(dev, hw_data);
	else if (data->type == CLK_MODEL_SYSBUS)
		ret = of_aml_clk_model_sysbus_init_register(dev, hw_data);

	if (clk_num == 1)
		return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
						   &clk->hw);
	else
		return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
						   hw_data);
}

static const struct aml_clk_model_data aml_composite_dev_data = {
	.type = CLK_MODEL_COMPOSITE,
};

static const struct aml_clk_model_data aml_noglitch_dev_data = {
	.type = CLK_MODEL_NOGLITCH,
};

static const struct aml_clk_model_data aml_sysbus_dev_data = {
	.type = CLK_MODEL_SYSBUS,
};

static const struct of_device_id of_aml_clk_model_match_table[] = {
	{
		.compatible = "amlogic,a9-composite-ccu",
		.data = &aml_composite_dev_data,
	},
	{
		.compatible = "amlogic,a9-composite-ccu-mult",
		.data = &aml_composite_dev_data,
	},
	{
		.compatible = "amlogic,a9-noglitch-ccu",
		.data = &aml_noglitch_dev_data,
	},
	{
		.compatible = "amlogic,a9-noglitch-ccu-mult",
		.data = &aml_noglitch_dev_data,
	},
	{
		.compatible = "amlogic,a9-sysbus-ccu",
		.data = &aml_sysbus_dev_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, of_aml_clk_model_match_table);

static struct platform_driver of_aml_clk_model_driver = {
	.probe		= of_aml_clk_model_probe,
	.driver		= {
		.name	= "aml-clk-model",
		.of_match_table = of_aml_clk_model_match_table,
	},
};

int __init aml_clk_model_driver_init(void)
{
	return platform_driver_register(&of_aml_clk_model_driver);
}

void __exit aml_clk_model_driver_exit(void)
{
	platform_driver_unregister(&of_aml_clk_model_driver);
}

MODULE_DESCRIPTION("Amlogic A9 Standardized Model Clock Control Units Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
