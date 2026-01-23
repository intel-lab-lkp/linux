// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic A5 PLL Controller Driver
 *
 * Copyright (c) 2024-2025 Amlogic, inc.
 * Author: Chuan Liu <chuan.liu@amlogic.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-mpll.h"
#include "meson-clkc-utils.h"
#include <dt-bindings/clock/amlogic,a5-pll-clkc.h>

#define GP0PLL_CTRL0			0x80
#define GP0PLL_CTRL1			0x84
#define GP0PLL_CTRL2			0x88
#define GP0PLL_CTRL3			0x8c
#define GP0PLL_CTRL4			0x90
#define GP0PLL_CTRL5			0x94
#define GP0PLL_CTRL6			0x98

#define HIFIPLL_CTRL0			0x100
#define HIFIPLL_CTRL1			0x104
#define HIFIPLL_CTRL2			0x108
#define HIFIPLL_CTRL3			0x10c
#define HIFIPLL_CTRL4			0x110
#define HIFIPLL_CTRL5			0x114
#define HIFIPLL_CTRL6			0x118

#define MPLL_CTRL0			0x180
#define MPLL_CTRL1			0x184
#define MPLL_CTRL2			0x188
#define MPLL_CTRL3			0x18c
#define MPLL_CTRL4			0x190
#define MPLL_CTRL5			0x194
#define MPLL_CTRL6			0x198
#define MPLL_CTRL7			0x19c
#define MPLL_CTRL8			0x1a0

static struct clk_fixed_factor a5_mpll_prediv = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mpll_prediv",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "fix_dco"
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence a5_mpll0_init_regs[] = {
	{ .reg = MPLL_CTRL2,	.def = 0x40000033 },
};

static struct clk_regmap a5_mpll0_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = MPLL_CTRL1,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = MPLL_CTRL1,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = MPLL_CTRL1,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = MPLL_CTRL1,
			.shift   = 29,
			.width	 = 1,
		},
		.init_regs = a5_mpll0_init_regs,
		.init_count = ARRAY_SIZE(a5_mpll0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a5_mpll0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = MPLL_CTRL1,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &a5_mpll0_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a5_mpll1_init_regs[] = {
	{ .reg = MPLL_CTRL4,	.def = 0x40000033 },
};

static struct clk_regmap a5_mpll1_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = MPLL_CTRL3,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = MPLL_CTRL3,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = MPLL_CTRL3,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = MPLL_CTRL3,
			.shift   = 29,
			.width	 = 1,
		},
		.init_regs = a5_mpll1_init_regs,
		.init_count = ARRAY_SIZE(a5_mpll1_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a5_mpll1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = MPLL_CTRL3,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "a5_mpll1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &a5_mpll1_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a5_mpll2_init_regs[] = {
	{ .reg = MPLL_CTRL6,	.def = 0x40000033 },
};

static struct clk_regmap a5_mpll2_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = MPLL_CTRL5,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = MPLL_CTRL5,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = MPLL_CTRL5,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = MPLL_CTRL5,
			.shift   = 29,
			.width	 = 1,
		},
		.init_regs = a5_mpll2_init_regs,
		.init_count = ARRAY_SIZE(a5_mpll2_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a5_mpll2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = MPLL_CTRL5,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &a5_mpll2_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a5_mpll3_init_regs[] = {
	{ .reg = MPLL_CTRL8,	.def = 0x40000033 },
};

static struct clk_regmap a5_mpll3_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = MPLL_CTRL7,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = MPLL_CTRL7,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = MPLL_CTRL7,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = MPLL_CTRL7,
			.shift   = 29,
			.width	 = 1,
		},
		.init_regs = a5_mpll3_init_regs,
		.init_count = ARRAY_SIZE(a5_mpll3_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a5_mpll3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = MPLL_CTRL7,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &a5_mpll3_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a5_gp0_init_regs[] = {
	{ .reg = GP0PLL_CTRL3, .def = 0x6a295c00 },
	{ .reg = GP0PLL_CTRL4, .def = 0x65771290 },
	{ .reg = GP0PLL_CTRL5, .def = 0x3927200a },
	{ .reg = GP0PLL_CTRL6, .def = 0x54540000 }
};

static const struct pll_mult_range a5_gp0_pll_mult_range = {
	.min = 125,
	.max = 250,
};

static struct clk_regmap a5_gp0_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.frac = {
			.reg_off = GP0PLL_CTRL1,
			.shift   = 0,
			.width   = 17,
		},
		.n = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.l = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.range = &a5_gp0_pll_mult_range,
		.init_regs = a5_gp0_init_regs,
		.init_count = ARRAY_SIZE(a5_gp0_init_regs),
		.frac_max = 100000,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

/* The maximum frequency divider supports is 32, not 128(2^7) */
static const struct clk_div_table a5_gp0_pll_od_table[] = {
	{ 0,  1 },
	{ 1,  2 },
	{ 2,  4 },
	{ 3,  8 },
	{ 4, 16 },
	{ 5, 32 },
	{ /* sentinel */ }
};

static struct clk_regmap a5_gp0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = GP0PLL_CTRL0,
		.shift = 16,
		.width = 3,
		.table = a5_gp0_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_gp0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a5_hifi_init_regs[] = {
	{ .reg = HIFIPLL_CTRL3, .def = 0x6a285c00 },
	{ .reg = HIFIPLL_CTRL4, .def = 0x65771290 },
	{ .reg = HIFIPLL_CTRL5, .def = 0x3927200a },
	{ .reg = HIFIPLL_CTRL6, .def = 0x56540000 }
};

static const struct pll_mult_range a5_hifi_pll_mult_range = {
	.min = 125,
	.max = 250,
};

static struct clk_regmap a5_hifi_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.frac = {
			.reg_off = HIFIPLL_CTRL1,
			.shift   = 0,
			.width   = 17,
		},
		.n = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.l = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.range = &a5_hifi_pll_mult_range,
		.init_regs = a5_hifi_init_regs,
		.init_count = ARRAY_SIZE(a5_hifi_init_regs),
		.frac_max = 100000,
		.flags = CLK_MESON_PLL_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a5_hifi_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = HIFIPLL_CTRL0,
		.shift = 16,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a5_hifi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_hw *a5_pll_hw_clks[] = {
	[CLKID_MPLL_PREDIV]	= &a5_mpll_prediv.hw,
	[CLKID_MPLL0_DIV]	= &a5_mpll0_div.hw,
	[CLKID_MPLL0]		= &a5_mpll0.hw,
	[CLKID_MPLL1_DIV]	= &a5_mpll1_div.hw,
	[CLKID_MPLL1]		= &a5_mpll1.hw,
	[CLKID_MPLL2_DIV]	= &a5_mpll2_div.hw,
	[CLKID_MPLL2]		= &a5_mpll2.hw,
	[CLKID_MPLL3_DIV]	= &a5_mpll3_div.hw,
	[CLKID_MPLL3]		= &a5_mpll3.hw,
	[CLKID_GP0_PLL_DCO]	= &a5_gp0_pll_dco.hw,
	[CLKID_GP0_PLL]		= &a5_gp0_pll.hw,
	[CLKID_HIFI_PLL_DCO]	= &a5_hifi_pll_dco.hw,
	[CLKID_HIFI_PLL]	= &a5_hifi_pll.hw
};

static const struct meson_clkc_data a5_pll_clkc_data = {
	.hw_clks = {
		.hws = a5_pll_hw_clks,
		.num = ARRAY_SIZE(a5_pll_hw_clks),
	},
};

static const struct of_device_id a5_pll_clkc_match_table[] = {
	{
		.compatible = "amlogic,a5-pll-clkc",
		.data = &a5_pll_clkc_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, a5_pll_clkc_match_table);

static struct platform_driver a5_pll_clkc_driver = {
	.probe = meson_clkc_mmio_probe,
	.driver = {
		.name = "a5-pll-clkc",
		.of_match_table = a5_pll_clkc_match_table,
	},
};
module_platform_driver(a5_pll_clkc_driver);

MODULE_DESCRIPTION("Amlogic A5 PLL Clock Controller driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_MESON");
