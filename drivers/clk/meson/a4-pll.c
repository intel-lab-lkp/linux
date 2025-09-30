// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic A4 PLL Controller Driver
 *
 * Copyright (c) 2025 Amlogic, inc.
 * Author: Chuan Liu <chuan.liu@amlogic.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include "clk-regmap.h"
#include "clk-pll.h"
#include "meson-clkc-utils.h"
#include <dt-bindings/clock/amlogic,a4-pll-clkc.h>

#define GP0PLL_CTRL0			0x80
#define GP0PLL_CTRL1			0x84
#define GP0PLL_CTRL2			0x88
#define GP0PLL_CTRL3			0x8c
#define HIFIPLL_CTRL0			0x100
#define HIFIPLL_CTRL1			0x104
#define HIFIPLL_CTRL2			0x108
#define HIFIPLL_CTRL3			0x10c

static const struct reg_sequence a4_gp0_init_regs[] = {
	{ .reg = GP0PLL_CTRL1, .def = 0x03a00000 },
	{ .reg = GP0PLL_CTRL2, .def = 0x00040000 },
	{ .reg = GP0PLL_CTRL3, .def = 0x090da200 }
};

static const struct pll_mult_range a4_gp0_pll_mult_range = {
	.min = 67,
	.max = 133,
};

static struct clk_regmap a4_gp0_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.frac = {
			.reg_off = GP0PLL_CTRL1,
			.shift   = 0,
			.width   = 17,
		},
		.n = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 16,
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
		.l_detect = {
			.reg_off = GP0PLL_CTRL3,
			.shift   = 9,
			.width   = 1,
		},
		.range = &a4_gp0_pll_mult_range,
		.init_regs = a4_gp0_init_regs,
		.init_count = ARRAY_SIZE(a4_gp0_init_regs),
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

/* The maximum frequency divider supports is 16, not 128(2^7) */
static const struct clk_div_table a4_gp0_pll_od_table[] = {
	{ 0,  1 },
	{ 1,  2 },
	{ 2,  4 },
	{ 3,  8 },
	{ 4, 16 },
	{ /* sentinel */ }
};

static struct clk_regmap a4_gp0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = GP0PLL_CTRL0,
		.shift = 10,
		.width = 3,
		.table = a4_gp0_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a4_gp0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence a4_hifi_init_regs[] = {
	{ .reg = HIFIPLL_CTRL1, .def = 0x03a00000 },
	{ .reg = HIFIPLL_CTRL2, .def = 0x00040000 },
	{ .reg = HIFIPLL_CTRL3, .def = 0x0a0da200 }
};

static const struct pll_mult_range a4_hifi_pll_mult_range = {
	.min = 67,
	.max = 133,
};

static struct clk_regmap a4_hifi_pll_dco = {
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
			.shift   = 16,
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
		.l_detect = {
			.reg_off = HIFIPLL_CTRL3,
			.shift   = 9,
			.width   = 1,
		},
		.range = &a4_hifi_pll_mult_range,
		.init_regs = a4_hifi_init_regs,
		.init_count = ARRAY_SIZE(a4_hifi_init_regs),
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

/* The maximum frequency divider supports is 16, not 128(2^7) */
static const struct clk_div_table a4_hifi_pll_od_table[] = {
	{ 0,  1 },
	{ 1,  2 },
	{ 2,  4 },
	{ 3,  8 },
	{ 4, 16 },
	{ /* sentinel */ }
};

static struct clk_regmap a4_hifi_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = HIFIPLL_CTRL0,
		.shift = 10,
		.width = 3,
		.table = a4_hifi_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a4_hifi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_hw *a4_pll_hw_clks[] = {
	[CLKID_GP0_PLL_DCO]	= &a4_gp0_pll_dco.hw,
	[CLKID_GP0_PLL]		= &a4_gp0_pll.hw,
	[CLKID_HIFI_PLL_DCO]	= &a4_hifi_pll_dco.hw,
	[CLKID_HIFI_PLL]	= &a4_hifi_pll.hw
};

static const struct meson_clkc_data a4_pll_clkc_data = {
	.hw_clks = {
		.hws = a4_pll_hw_clks,
		.num = ARRAY_SIZE(a4_pll_hw_clks),
	},
};

static const struct of_device_id a4_pll_clkc_match_table[] = {
	{
		.compatible = "amlogic,a4-pll-clkc",
		.data = &a4_pll_clkc_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, a4_pll_clkc_match_table);

static struct platform_driver a4_pll_clkc_driver = {
	.probe = meson_clkc_mmio_probe,
	.driver = {
		.name = "a4-pll-clkc",
		.of_match_table = a4_pll_clkc_match_table,
	},
};
module_platform_driver(a4_pll_clkc_driver);

MODULE_DESCRIPTION("Amlogic A4 PLL Clock Controller driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_MESON");
