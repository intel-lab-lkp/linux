// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ambarella, Inc.
 *
 * CV75 RCT clock controller for boot clocks.
 */

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/clock/ambarella,cv75-clock.h>

#include "ccu_common.h"
#include "ccu_mux_div.h"
#include "ccu_pll.h"

enum amb_cv75_clk_type {
	AMB_CV75_CLK_FIXED_RATE,
	AMB_CV75_CLK_FIXED_FACTOR,
	AMB_CV75_CLK_PLL,
	AMB_CV75_CLK_DIV,
	AMB_CV75_CLK_MUX_DIV,
};

enum amb_cv75_clk_ref {
	AMB_CV75_CLK_REF_OSC = -1,
	AMB_CV75_CLK_REF_DUMMY = -2,
};

/* PLL version for CV75 (ambarella,clkpll-v1 in vendor DTS) */
static const struct amb_pll_soc_data cv75_pll_soc_data = {
	.pll_version	= 1,
	.fsout_mask	= CTRL2_FSOUT_DIV2,
	.fsout_val	= CTRL2_FSOUT_DIV2,
	.fsdiv_mask	= CTRL2_FSDIV_DIV2,
	.fsdiv_val	= CTRL2_FSDIV_DIV2,
	.vcodiv_mask	= CTRL2_VCODIV_DIV2,
	.vcodiv_val	= CTRL2_VCODIV_DIV2,
	.vco_max_mhz	= 2600UL,
	.vco_min_mhz	= 850UL,
	.vco_range	= { 1800UL, 1400UL, 1100UL, 0UL },
};

struct amb_cv75_clk_desc {
	int id;
	enum amb_cv75_clk_type type;
	const char *name;
	int parent;

	union {
		struct {
			unsigned long rate;
		} fixed_rate;
		struct {
			unsigned long flags;
			unsigned int mult;
			unsigned int div;
		} fixed_factor;
		struct {
			u32 reg_offset[REG_NUM];
			const struct amb_pll_soc_data *soc_data;
		} pll;
		struct {
			u32 reg;
			u32 shift;
			u32 width;
			u32 flags;
			u32 fix_divider;
		} div;
		struct amb_mux_div_desc mux_div;
	};
};

static const struct amb_cv75_clk_desc cv75_clks[] = {
	{
		.id = AMB_CV75_CLK_REF_DUMMY,
		.type = AMB_CV75_CLK_FIXED_RATE,
		.name = "dummy",
		.fixed_rate.rate = 0,
	},
	{
		.id = CV75_GCLK_CORE,
		.type = AMB_CV75_CLK_PLL,
		.name = "core",
		.parent = AMB_CV75_CLK_REF_OSC,
		.pll = {
			.reg_offset = {
				0x000, 0x004, 0x100,
				0x104, 0x000, 0x000,
			},
			.soc_data = &cv75_pll_soc_data,
		},
	},
	{
		.id = CV75_GCLK_AHB,
		.type = AMB_CV75_CLK_FIXED_FACTOR,
		.name = "ahb",
		.parent = CV75_GCLK_CORE,
		.fixed_factor = {
			.flags = 0,
			.mult = 1,
			.div = 2,
		},
	},
	{
		.id = CV75_GCLK_APB,
		.type = AMB_CV75_CLK_FIXED_FACTOR,
		.name = "apb",
		.parent = CV75_GCLK_CORE,
		.fixed_factor = {
			.flags = 0,
			.mult = 1,
			.div = 4,
		},
	},
	{
		.id = CV75_GCLK_UART0,
		.type = AMB_CV75_CLK_MUX_DIV,
		.name = "uart0",
		.mux_div = {
			.name = "uart0",
			.parents = (const int[]) {
				AMB_CV75_CLK_REF_OSC, CV75_GCLK_CORE,
				AMB_CV75_CLK_REF_DUMMY, AMB_CV75_CLK_REF_DUMMY,
			},
			.num_parents = 4,
			.mux_reg = 0x1c8,
			.mux_shift = 0,
			.mux_mask = 0x3,
			.div_reg = 0x038,
			.div_shift = 0,
			.div_width = 24,
			.div_flags = CLK_DIVIDER_ONE_BASED,
			.fix_divider = 1,
		},
	},
};

static struct clk_hw *amb_cv75_get_parent(struct amb_ccu *ccu,
					  struct clk_hw *osc,
					  struct clk_hw *dummy,
					  int parent)
{
	if (parent == AMB_CV75_CLK_REF_OSC)
		return osc;
	if (parent == AMB_CV75_CLK_REF_DUMMY)
		return dummy;

	if (parent < 0 || parent >= ccu->data->num)
		return ERR_PTR(-EINVAL);

	if (!ccu->data->hws[parent])
		return ERR_PTR(-EPROBE_DEFER);

	return ccu->data->hws[parent];
}

static struct clk_hw *amb_cv75_register_mux_div(struct device *dev,
						struct amb_ccu *ccu,
						const struct amb_cv75_clk_desc *desc,
						struct clk_hw *osc,
						struct clk_hw *dummy)
{
	const struct amb_mux_div_desc *md = &desc->mux_div;
	struct clk_parent_data *pdata;
	struct clk_hw *parent;
	u8 i;

	pdata = devm_kcalloc(dev, md->num_parents, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < md->num_parents; i++) {
		if (md->parents[i] == AMB_CV75_CLK_REF_OSC) {
			pdata[i].fw_name = "osc";
			continue;
		}

		parent = amb_cv75_get_parent(ccu, osc, dummy, md->parents[i]);
		if (IS_ERR(parent))
			return parent;

		pdata[i].hw = parent;
	}

	return amb_mux_div_register(dev, ccu->map, md, pdata);
}

static struct clk_hw *amb_cv75_register_clk(struct device *dev,
					    struct amb_ccu *ccu,
					    const struct amb_cv75_clk_desc *desc,
					    struct clk_hw *osc,
					    struct clk_hw *dummy)
{
	struct amb_pll_desc pll_desc;
	struct clk_hw *parent;

	switch (desc->type) {
	case AMB_CV75_CLK_FIXED_RATE:
		return devm_clk_hw_register_fixed_rate(dev, desc->name, NULL, 0,
						       desc->fixed_rate.rate);
	case AMB_CV75_CLK_FIXED_FACTOR:
		parent = amb_cv75_get_parent(ccu, osc, dummy, desc->parent);
		if (IS_ERR(parent))
			return parent;

		return devm_clk_hw_register_fixed_factor_parent_hw(dev,
				desc->name, parent,
				desc->fixed_factor.flags,
				desc->fixed_factor.mult,
				desc->fixed_factor.div);
	case AMB_CV75_CLK_PLL:
		parent = amb_cv75_get_parent(ccu, osc, dummy, desc->parent);
		if (IS_ERR(parent))
			return parent;

		pll_desc.name = desc->name;
		pll_desc.parent = parent;
		memcpy((void *)pll_desc.reg_offset, desc->pll.reg_offset,
		       sizeof(pll_desc.reg_offset));
		pll_desc.soc_data = desc->pll.soc_data;
		pll_desc.frac_mode = false;

		return amb_pll_register(dev, ccu->map, &pll_desc);
	case AMB_CV75_CLK_DIV:
		parent = amb_cv75_get_parent(ccu, osc, dummy, desc->parent);
		if (IS_ERR(parent))
			return parent;

		return amb_div_register(dev, ccu->map, desc->name, parent,
					desc->div.reg, desc->div.shift,
					desc->div.width, desc->div.flags,
					desc->div.fix_divider);
	case AMB_CV75_CLK_MUX_DIV:
		return amb_cv75_register_mux_div(dev, ccu, desc, osc, dummy);
	default:
		return ERR_PTR(-EINVAL);
	}
}

static int amb_cv75_rct_probe(struct platform_device *pdev)
{
	struct amb_ccu *ccu;
	struct clk *osc_clk;
	struct clk_hw *osc, *dummy = NULL, *hw;
	int i;

	ccu = amb_ccu_init(pdev, CV75_CLK_NUM);
	if (IS_ERR(ccu))
		return PTR_ERR(ccu);

	osc_clk = devm_clk_get(&pdev->dev, "osc");
	if (IS_ERR(osc_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(osc_clk),
				     "missing osc clock\n");
	osc = __clk_get_hw(osc_clk);

	for (i = 0; i < ARRAY_SIZE(cv75_clks); i++) {
		hw = amb_cv75_register_clk(&pdev->dev, ccu, &cv75_clks[i],
					   osc, dummy);
		if (IS_ERR(hw))
			return dev_err_probe(&pdev->dev, PTR_ERR(hw),
					     "failed to register %s\n",
					     cv75_clks[i].name);

		if (cv75_clks[i].id == AMB_CV75_CLK_REF_DUMMY)
			dummy = hw;
		else
			ccu->data->hws[cv75_clks[i].id] = hw;
	}

	return amb_ccu_register(ccu);
}

static const struct of_device_id amb_cv75_rct_match[] = {
	{ .compatible = "ambarella,cv75-rct" },
	{ }
};
MODULE_DEVICE_TABLE(of, amb_cv75_rct_match);

static struct platform_driver amb_cv75_rct_driver = {
	.probe	= amb_cv75_rct_probe,
	.driver = {
		.name = "ambarella-cv75-rct",
		.of_match_table = amb_cv75_rct_match,
	},
};
module_platform_driver(amb_cv75_rct_driver);

MODULE_AUTHOR("Ambarella Inc.");
MODULE_DESCRIPTION("Ambarella CV75 RCT clock controller");
MODULE_LICENSE("GPL");
