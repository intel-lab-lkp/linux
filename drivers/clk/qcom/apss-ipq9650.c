// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/interconnect-provider.h>

#include <dt-bindings/clock/qcom,apss-ipq.h>
#include <dt-bindings/interconnect/qcom,ipq9650.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-regmap.h"
#include "common.h"

enum {
	DT_XO,
	DT_GPLL0,
};

static const struct pll_vco silver_pll_vco[] = {
	{ 816000000, 1968000000, 0 },
};

static const struct pll_vco gold_pll_vco[] = {
	{ 696000000, 1992000000, 0 },
};

static const struct pll_vco l3_pll_vco[] = {
	{ 696000000, 1512000000, 0 },
};

static const struct alpha_pll_config silver_apss_pll_config = {
	.l = 0x23,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
};

static const struct alpha_pll_config gold_apss_pll_config = {
	.l = 0x26,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
};

static const struct alpha_pll_config l3_pll_config = {
	.l = 0x1e,
	.config_ctl_val = 0x08200920,
	.config_ctl_hi_val = 0x05008001,
	.config_ctl_hi1_val = 0x04000000,
	.user_ctl_val = 0x01000009,
};

static struct clk_alpha_pll silver_apss_pll = {
	.offset = 0x0,
	.config = &silver_apss_pll_config,
	.vco_table = silver_pll_vco,
	.num_vco = ARRAY_SIZE(silver_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = 0x0,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "silver_apss_pll",
			.parent_data = &(const struct clk_parent_data){
				.index = DT_XO,
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

static struct clk_alpha_pll gold_apss_pll = {
	.offset = 0x10000,
	.config = &gold_apss_pll_config,
	.vco_table = gold_pll_vco,
	.num_vco = ARRAY_SIZE(gold_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = 0x10000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "gold_apss_pll",
			.parent_data = &(const struct clk_parent_data){
				.index = DT_XO,
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

static const struct clk_div_table gold_apss_pll_post_div_table[] = {
	{ 0x0, 1 },
	{ 0x1, 2 },
	{ }
};

static struct clk_alpha_pll_postdiv gold_apss_pll_postdiv = {
	.offset = 0x10000,
	.post_div_shift = 8,
	.post_div_table = gold_apss_pll_post_div_table,
	.num_post_div = ARRAY_SIZE(gold_apss_pll_post_div_table),
	.width = 2,
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.clkr.hw.init = &(struct clk_init_data){
		.name = "gold_apss_pll_postdiv",
		.parent_hws = (const struct clk_hw*[]){
			&gold_apss_pll.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_alpha_pll_postdiv_zonda_ops,
	},
};

static struct clk_alpha_pll l3_pll = {
	.offset = 0x20000,
	.config = &l3_pll_config,
	.vco_table = l3_pll_vco,
	.num_vco = ARRAY_SIZE(l3_pll_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_ZONDA],
	.flags = SUPPORTS_DYNAMIC_UPDATE,
	.clkr = {
		.enable_reg = 0x20000,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data){
			.name = "l3_pll",
			.parent_data = &(const struct clk_parent_data){
				.index = DT_XO,
			},
			.num_parents = 1,
			.flags = CLK_IS_CRITICAL,
			.ops = &clk_alpha_pll_zonda_ops,
		},
	},
};

struct clk_gfm {
	u32 mux_reg;
	u32 mux_mask;
	struct clk_hw hw;
	void __iomem *base;
};

#define to_clk_gfm(_hw) container_of(_hw, struct clk_gfm, hw)

static u8 clk_gfm_get_parent(struct clk_hw *hw)
{
	struct clk_gfm *gfm = to_clk_gfm(hw);
	u32 val;

	val = readl(gfm->base + gfm->mux_reg);
	return val & gfm->mux_mask;
}

static int clk_gfm_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_gfm *gfm = to_clk_gfm(hw);
	u32 val;

	val = readl(gfm->base + gfm->mux_reg);
	val &= ~gfm->mux_mask;
	val |= (index & gfm->mux_mask);
	writel(val, gfm->base + gfm->mux_reg);

	return 0;
}

static const struct clk_ops clk_gfm_ops = {
	.get_parent = clk_gfm_get_parent,
	.set_parent = clk_gfm_set_parent,
	.determine_rate = __clk_mux_determine_rate_closest,
};

static struct clk_gfm silver_apss_pll_gfmux = {
	.mux_reg = 0x84,
	.mux_mask = 0xf,
	.hw.init = &(struct clk_init_data) {
		.name = "silver_apss_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]) {
			{ .index = DT_XO },
			{ .index = DT_GPLL0 },
			{ .hw = &silver_apss_pll.clkr.hw },
		},
		.num_parents = 3,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_gfm gold_apss_pll_gfmux = {
	.mux_reg = 0x10084,
	.mux_mask = 0xf,
	.hw.init = &(struct clk_init_data) {
		.name = "gold_apss_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]) {
			{ .index = DT_XO },
			{ .index = DT_GPLL0 },
			{ .hw = &gold_apss_pll.clkr.hw },
			{ .hw = &gold_apss_pll_postdiv.clkr.hw },
		},
		.num_parents = 4,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_gfm l3_pll_gfmux = {
	.mux_reg = 0x20084,
	.mux_mask = 0xf,
	.hw.init = &(struct clk_init_data) {
		.name = "l3_pll_gfmux",
		.parent_data = (const struct clk_parent_data[]) {
			{ .index = DT_XO },
			{ .index = DT_GPLL0 },
			{ .hw = &l3_pll.clkr.hw },
		},
		.num_parents = 3,
		.ops = &clk_gfm_ops,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_branch apss_silver_core_clk = {
	.halt_reg = 0x8c,
	.clkr = {
		.enable_reg = 0x8c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data) {
			.name = "apss_silver_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&silver_apss_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch apss_gold_core_clk = {
	.halt_reg = 0x1008c,
	.clkr = {
		.enable_reg = 0x1008c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data) {
			.name = "apss_gold_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&gold_apss_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch l3_core_clk = {
	.halt_reg = 0x2008c,
	.clkr = {
		.enable_reg = 0x2008c,
		.enable_mask = BIT(0),
		.hw.init = &(struct clk_init_data) {
			.name = "l3_core_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&l3_pll_gfmux.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_hw *apss_hws[] = {
	&silver_apss_pll_gfmux.hw,
	&l3_pll_gfmux.hw,
	&gold_apss_pll_gfmux.hw,
};

static struct clk_regmap *apss_clks[] = {
	[APSS_PLL_EARLY] = &silver_apss_pll.clkr,
	[APSS_SILVER_CORE_CLK] = &apss_silver_core_clk.clkr,
	[L3_PLL] = &l3_pll.clkr,
	[L3_CORE_CLK] = &l3_core_clk.clkr,
	[APSS_GOLD_PLL] = &gold_apss_pll.clkr,
	[APSS_GOLD_PLL_POSTDIV] = &gold_apss_pll_postdiv.clkr,
	[APSS_GOLD_CORE_CLK] = &apss_gold_core_clk.clkr,
};

static const struct regmap_config apss_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x2fffc,
	.fast_io = true,
};

static struct clk_alpha_pll *ipq9650_apss_plls[] = {
	&silver_apss_pll,
	&gold_apss_pll,
	&l3_pll,
};

static const struct qcom_cc_driver_data ipq9650_apss_driver_data = {
	.alpha_plls = ipq9650_apss_plls,
	.num_alpha_plls = ARRAY_SIZE(ipq9650_apss_plls),
};

#define IPQ_APPS_PLL_ID			(9650 * 3)	/* some unique value */

static const struct qcom_icc_hws_data icc_cpu_l3[] = {
	{ MASTER_CPU_SILVER, SLAVE_L3, L3_CORE_CLK },
	{ MASTER_CPU_GOLD, SLAVE_L3, L3_CORE_CLK },
};

static const struct qcom_cc_desc apss_desc = {
	.config = &apss_regmap_config,
	.clks = apss_clks,
	.num_clks = ARRAY_SIZE(apss_clks),
	.icc_hws = icc_cpu_l3,
	.num_icc_hws = ARRAY_SIZE(icc_cpu_l3),
	.icc_first_node_id = IPQ_APPS_PLL_ID,
	.clk_hws = apss_hws,
	.num_clk_hws = ARRAY_SIZE(apss_hws),
	.driver_data = &ipq9650_apss_driver_data,
};

static int apss_ipq9650_probe(struct platform_device *pdev)
{
	const struct qcom_cc_desc *desc = &apss_desc;
	struct regmap *regmap;
	struct clk_gfm *gfm;
	void __iomem *base;
	int i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	regmap = devm_regmap_init_mmio(&pdev->dev, base,
				       &apss_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	for (i = 0; i < desc->num_clk_hws; i++) {
		gfm = to_clk_gfm(apss_hws[i]);
		gfm->base = base;
	}

	return qcom_cc_really_probe(&pdev->dev, desc, regmap);
}

static const struct of_device_id apss_ipq9650_match_table[] = {
	{ .compatible = "qcom,ipq9650-apss-clk" },
	{ }
};
MODULE_DEVICE_TABLE(of, apss_ipq9650_match_table);

static struct platform_driver apss_ipq9650_driver = {
	.probe = apss_ipq9650_probe,
	.driver = {
		.name = "qcom,ipq9650-apss-clk",
		.of_match_table = apss_ipq9650_match_table,
		.sync_state = icc_sync_state,
	},
};

module_platform_driver(apss_ipq9650_driver);

MODULE_DESCRIPTION("Qualcomm IPQ9650 APSS Clock Driver");
MODULE_LICENSE("GPL");
