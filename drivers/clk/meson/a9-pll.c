// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/amlogic,a9-pll-clkc.h>
#include "clk-regmap.h"
#include "clk-pll.h"
#include "meson-clkc-utils.h"

#define GP0PLL_CTRL0			0x00
#define GP0PLL_CTRL1			0x04
#define GP0PLL_CTRL2			0x08
#define GP0PLL_CTRL3			0x0c
#define GP0PLL_CTRL4			0x10

/* HIFI0 and HIFI1 share the same IP and register offset layout. */
#define HIFIPLL_CTRL0			0x00
#define HIFIPLL_CTRL1			0x04
#define HIFIPLL_CTRL2			0x08
#define HIFIPLL_CTRL3			0x0c
#define HIFIPLL_CTRL4			0x10

/* MCLK0 and MCLK1 share the same IP and register offset layout. */
#define MCLKPLL_CTRL0			0x00
#define MCLKPLL_CTRL1			0x04
#define MCLKPLL_CTRL2			0x08
#define MCLKPLL_CTRL3			0x0c
#define MCLKPLL_CTRL4			0x10

#define A9_COMP_SEL(_name, _reg, _shift, _mask, _pdata) \
	MESON_COMP_SEL(a9_, _name, _reg, _shift, _mask, _pdata, NULL, 0, 0)

#define A9_COMP_DIV(_name, _reg, _shift, _width) \
	MESON_COMP_DIV(a9_, _name, _reg, _shift, _width, 0, CLK_SET_RATE_PARENT)

#define A9_COMP_GATE(_name, _reg, _bit) \
	MESON_COMP_GATE(a9_, _name, _reg, _bit, CLK_SET_RATE_PARENT)

/*
 * Compared with previous SoC PLLs, the A9 PLL input path has an inherent
 * 2-divider. The N pre-divider follows the same calculation rule as OD,
 * where the pre-divider ratio equals 2^N.
 *
 * A9 PLL is composed as follows:
 *
 *                      PLL
 *         +---------------------------------+
 *         |                                 |
 *         |             +--+                |
 *  in/2 >>---[ /2^N ]-->|  |      +-----+   |
 *         |             |  |------| DCO |----->> out
 *         |  +--------->|  |      +--v--+   |
 *         |  |          +--+         |      |
 *         |  |                       |      |
 *         |  +--[ *(M + (F/Fmax) ]<--+      |
 *         |                                 |
 *         +---------------------------------+
 *
 * out = in / 2  * (m + frac / frac_max) / 2^n
 */

static struct clk_fixed_factor a9_gp0_in_div2_div = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "gp0_in_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "in0",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_gp0_in_div2 = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = GP0PLL_CTRL0,
		.bit_idx = 27,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_in_div2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_gp0_in_div2_div.hw
		},
		.num_parents = 1,
	},
};

/* The output frequency range of the A9 PLL_DCO is 1.4 GHz to 2.8 GHz. */
static const struct pll_mult_range a9_pll_mult_range = {
	.min = 117,
	.max = 233,
};

static const struct reg_sequence a9_gp0_pll_init_regs[] = {
	{ .reg = GP0PLL_CTRL0, .def = 0x00010000 },
	{ .reg = GP0PLL_CTRL1, .def = 0x11480000 },
	{ .reg = GP0PLL_CTRL2, .def = 0x1219b010 },
	{ .reg = GP0PLL_CTRL3, .def = 0x00008010 }
};

static struct clk_regmap a9_gp0_pll_dco = {
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
		.n = {
			.reg_off = GP0PLL_CTRL0,
			.shift   = 12,
			.width   = 3,
		},
		.frac = {
			.reg_off = GP0PLL_CTRL1,
			.shift   = 0,
			.width   = 17,
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
			.reg_off = GP0PLL_CTRL0,
			.shift   = 30,
			.width   = 1,
		},
		.range = &a9_pll_mult_range,
		.init_regs = a9_gp0_pll_init_regs,
		.init_count = ARRAY_SIZE(a9_gp0_pll_init_regs),
		.flags = CLK_MESON_PLL_RST_ACTIVE_LOW |
			 CLK_MESON_PLL_N_POWER_OF_TWO |
			 CLK_MESON_PLL_L_DETECT_ACTIVE_HIGH,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_gp0_in_div2.hw
		},
		.num_parents = 1,
	},
};

/* For gp0, hifi and mclk pll, the maximum value of od is 4. */
static const struct clk_div_table a9_pll_od_table[] = {
	{ 0,  1 },
	{ 1,  2 },
	{ 2,  4 },
	{ 3,  8 },
	{ 4,  16 },
	{ /* sentinel */ }
};

static struct clk_regmap a9_gp0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = GP0PLL_CTRL0,
		.shift = 20,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_gp0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor a9_hifi0_in_div2_div = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "hifi0_in_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "in0",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_hifi0_in_div2 = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = HIFIPLL_CTRL0,
		.bit_idx = 27,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi0_in_div2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi0_in_div2_div.hw
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence a9_hifi0_pll_init_regs[] = {
	{ .reg = HIFIPLL_CTRL0, .def = 0x00010000 },
	{ .reg = HIFIPLL_CTRL1, .def = 0x11480000 },
	{ .reg = HIFIPLL_CTRL2, .def = 0x1219b010 },
	{ .reg = HIFIPLL_CTRL3, .def = 0x00008010 }
};

static struct clk_regmap a9_hifi0_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.n = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 12,
			.width   = 3,
		},
		.frac = {
			.reg_off = HIFIPLL_CTRL1,
			.shift   = 0,
			.width   = 17,
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
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 30,
			.width   = 1,
		},
		.range = &a9_pll_mult_range,
		.init_regs = a9_hifi0_pll_init_regs,
		.init_count = ARRAY_SIZE(a9_hifi0_pll_init_regs),
		.frac_max = 100000,
		.flags = CLK_MESON_PLL_RST_ACTIVE_LOW |
			 CLK_MESON_PLL_N_POWER_OF_TWO |
			 CLK_MESON_PLL_L_DETECT_ACTIVE_HIGH,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi0_in_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_hifi0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = HIFIPLL_CTRL0,
		.shift = 20,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor a9_hifi1_in_div2_div = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "hifi1_in_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "in0",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_hifi1_in_div2 = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = HIFIPLL_CTRL0,
		.bit_idx = 27,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi1_in_div2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi1_in_div2_div.hw
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence a9_hifi1_pll_init_regs[] = {
	{ .reg = HIFIPLL_CTRL0, .def = 0x00010000 },
	{ .reg = HIFIPLL_CTRL1, .def = 0x11480000 },
	{ .reg = HIFIPLL_CTRL2, .def = 0x1219b011 },
	{ .reg = HIFIPLL_CTRL3, .def = 0x00008010 }
};

static struct clk_regmap a9_hifi1_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.n = {
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 12,
			.width   = 3,
		},
		.frac = {
			.reg_off = HIFIPLL_CTRL1,
			.shift   = 0,
			.width   = 17,
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
			.reg_off = HIFIPLL_CTRL0,
			.shift   = 30,
			.width   = 1,
		},
		.range = &a9_pll_mult_range,
		.init_regs = a9_hifi1_pll_init_regs,
		.init_count = ARRAY_SIZE(a9_hifi1_pll_init_regs),
		.frac_max = 100000,
		.flags = CLK_MESON_PLL_RST_ACTIVE_LOW |
			 CLK_MESON_PLL_N_POWER_OF_TWO |
			 CLK_MESON_PLL_L_DETECT_ACTIVE_HIGH,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi1_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi1_in_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_hifi1_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = HIFIPLL_CTRL0,
		.shift = 20,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_hifi1_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * Unlike GP0 and HIFI PLLs, the input divider 2 of MCLK PLL is
 * enabled by default and has no enable control bit.
 */
static struct clk_fixed_factor a9_mclk0_in_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk0_in_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "in0",
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence a9_mclk0_pll_init_regs[] = {
	{ .reg = MCLKPLL_CTRL1, .def = 0x00422000 },
	{ .reg = MCLKPLL_CTRL2, .def = 0x60000100 },
	{ .reg = MCLKPLL_CTRL3, .def = 0x02000200 },
	{ .reg = MCLKPLL_CTRL4, .def = 0xd616d616 }
};

static struct clk_regmap a9_mclk0_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.n = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 12,
			.width   = 3,
		},
		.l = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.l_detect = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 30,
			.width   = 1,
		},
		.range = &a9_pll_mult_range,
		.init_regs = a9_mclk0_pll_init_regs,
		.init_count = ARRAY_SIZE(a9_mclk0_pll_init_regs),
		.flags = CLK_MESON_PLL_RST_ACTIVE_LOW |
			 CLK_MESON_PLL_N_POWER_OF_TWO |
			 CLK_MESON_PLL_L_DETECT_ACTIVE_HIGH,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_in_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk0_0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 0,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk0_0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk0_0_pre = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 3,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk0_0_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_0_pll.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data a9_mclk0_0_parents[] = {
	{ .hw = &a9_mclk0_0_pre.hw },
	{ .fw_name = "in0" },
	{ .fw_name = "in1" },
	{ .fw_name = "in2" }
};

static A9_COMP_SEL(mclk0_0, MCLKPLL_CTRL3, 12, 0x3, a9_mclk0_0_parents);
static A9_COMP_DIV(mclk0_0, MCLKPLL_CTRL3, 10, 1);
static A9_COMP_GATE(mclk0_0, MCLKPLL_CTRL3, 8);

static struct clk_regmap a9_mclk0_1_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 16,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk0_1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk0_1_pre = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 19,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk0_1_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk0_1_pll.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data a9_mclk0_1_parents[] = {
	{ .hw = &a9_mclk0_1_pre.hw },
	{ .fw_name = "in0" },
	{ .fw_name = "in1" },
	{ .fw_name = "in2" }
};

static A9_COMP_SEL(mclk0_1, MCLKPLL_CTRL3, 28, 0x3, a9_mclk0_1_parents);
static A9_COMP_DIV(mclk0_1, MCLKPLL_CTRL3, 26, 1);
static A9_COMP_GATE(mclk0_1, MCLKPLL_CTRL3, 24);

static struct clk_fixed_factor a9_mclk1_in_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk1_in_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "in0",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk1_pll_dco = {
	.data = &(struct meson_clk_pll_data) {
		.en = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.n = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 12,
			.width   = 3,
		},
		.l = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.l_detect = {
			.reg_off = MCLKPLL_CTRL0,
			.shift   = 30,
			.width   = 1,
		},
		.range = &a9_pll_mult_range,
		.init_regs = a9_mclk0_pll_init_regs,
		.init_count = ARRAY_SIZE(a9_mclk0_pll_init_regs),
		.flags = CLK_MESON_PLL_RST_ACTIVE_LOW |
			 CLK_MESON_PLL_N_POWER_OF_TWO |
			 CLK_MESON_PLL_L_DETECT_ACTIVE_HIGH,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk1_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_in_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk1_0_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 0,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk1_0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk1_0_pre = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 3,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk1_0_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_0_pll.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data a9_mclk1_0_parents[] = {
	{ .hw = &a9_mclk1_0_pre.hw },
	{ .fw_name = "in0" },
	{ .fw_name = "in1" },
	{ .fw_name = "in2" }
};

static A9_COMP_SEL(mclk1_0, MCLKPLL_CTRL3, 12, 0x3, a9_mclk1_0_parents);
static A9_COMP_DIV(mclk1_0, MCLKPLL_CTRL3, 10, 1);
static A9_COMP_GATE(mclk1_0, MCLKPLL_CTRL3, 8);

static struct clk_regmap a9_mclk1_1_pll = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 16,
		.width = 3,
		.table = a9_pll_od_table,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk1_1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap a9_mclk1_1_pre = {
	.data = &(struct clk_regmap_div_data) {
		.offset = MCLKPLL_CTRL3,
		.shift = 19,
		.width = 5,
		.flags = CLK_DIVIDER_MAX_AT_ZERO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk1_1_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&a9_mclk1_1_pll.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data a9_mclk1_1_parents[] = {
	{ .hw = &a9_mclk1_1_pre.hw },
	{ .fw_name = "in0" },
	{ .fw_name = "in1" },
	{ .fw_name = "in2" }
};

static A9_COMP_SEL(mclk1_1, MCLKPLL_CTRL3, 28, 0x3, a9_mclk1_1_parents);
static A9_COMP_DIV(mclk1_1, MCLKPLL_CTRL3, 26, 1);
static A9_COMP_GATE(mclk1_1, MCLKPLL_CTRL3, 24);

static struct clk_hw *a9_gp0_hw_clks[] = {
	[CLKID_GP0_IN_DIV2_DIV]		= &a9_gp0_in_div2_div.hw,
	[CLKID_GP0_IN_DIV2]		= &a9_gp0_in_div2.hw,
	[CLKID_GP0_PLL_DCO]		= &a9_gp0_pll_dco.hw,
	[CLKID_GP0_PLL]			= &a9_gp0_pll.hw,
};

static struct clk_hw *a9_hifi0_hw_clks[] = {
	[CLKID_HIFI0_IN_DIV2_DIV]	= &a9_hifi0_in_div2_div.hw,
	[CLKID_HIFI0_IN_DIV2]		= &a9_hifi0_in_div2.hw,
	[CLKID_HIFI0_PLL_DCO]		= &a9_hifi0_pll_dco.hw,
	[CLKID_HIFI0_PLL]		= &a9_hifi0_pll.hw,
};

static struct clk_hw *a9_hifi1_hw_clks[] = {
	[CLKID_HIFI1_IN_DIV2_DIV]	= &a9_hifi1_in_div2_div.hw,
	[CLKID_HIFI1_IN_DIV2]		= &a9_hifi1_in_div2.hw,
	[CLKID_HIFI1_PLL_DCO]		= &a9_hifi1_pll_dco.hw,
	[CLKID_HIFI1_PLL]		= &a9_hifi1_pll.hw,
};

static struct clk_hw *a9_mclk0_hw_clks[] = {
	[CLKID_MCLK0_IN_DIV2]		= &a9_mclk0_in_div2.hw,
	[CLKID_MCLK0_PLL_DCO]		= &a9_mclk0_pll_dco.hw,
	[CLKID_MCLK0_0_PLL]		= &a9_mclk0_0_pll.hw,
	[CLKID_MCLK0_0_PRE]		= &a9_mclk0_0_pre.hw,
	[CLKID_MCLK0_0_SEL]		= &a9_mclk0_0_sel.hw,
	[CLKID_MCLK0_0_DIV]		= &a9_mclk0_0_div.hw,
	[CLKID_MCLK0_0]			= &a9_mclk0_0.hw,
	[CLKID_MCLK0_1_PLL]		= &a9_mclk0_1_pll.hw,
	[CLKID_MCLK0_1_PRE]		= &a9_mclk0_1_pre.hw,
	[CLKID_MCLK0_1_SEL]		= &a9_mclk0_1_sel.hw,
	[CLKID_MCLK0_1_DIV]		= &a9_mclk0_1_div.hw,
	[CLKID_MCLK0_1]			= &a9_mclk0_1.hw,
};

static struct clk_hw *a9_mclk1_hw_clks[] = {
	[CLKID_MCLK1_IN_DIV2]		= &a9_mclk1_in_div2.hw,
	[CLKID_MCLK1_PLL_DCO]		= &a9_mclk1_pll_dco.hw,
	[CLKID_MCLK1_0_PLL]		= &a9_mclk1_0_pll.hw,
	[CLKID_MCLK1_0_PRE]		= &a9_mclk1_0_pre.hw,
	[CLKID_MCLK1_0_SEL]		= &a9_mclk1_0_sel.hw,
	[CLKID_MCLK1_0_DIV]		= &a9_mclk1_0_div.hw,
	[CLKID_MCLK1_0]			= &a9_mclk1_0.hw,
	[CLKID_MCLK1_1_PLL]		= &a9_mclk1_1_pll.hw,
	[CLKID_MCLK1_1_PRE]		= &a9_mclk1_1_pre.hw,
	[CLKID_MCLK1_1_SEL]		= &a9_mclk1_1_sel.hw,
	[CLKID_MCLK1_1_DIV]		= &a9_mclk1_1_div.hw,
	[CLKID_MCLK1_1]			= &a9_mclk1_1.hw,
};

static const struct meson_clkc_data a9_gp0_data = {
	.hw_clks = {
		.hws = a9_gp0_hw_clks,
		.num = ARRAY_SIZE(a9_gp0_hw_clks),
	},
};

static const struct meson_clkc_data a9_hifi0_data = {
	.hw_clks = {
		.hws = a9_hifi0_hw_clks,
		.num = ARRAY_SIZE(a9_hifi0_hw_clks),
	},
};

static const struct meson_clkc_data a9_hifi1_data = {
	.hw_clks = {
		.hws = a9_hifi1_hw_clks,
		.num = ARRAY_SIZE(a9_hifi1_hw_clks),
	},
};

static const struct meson_clkc_data a9_mclk0_data = {
	.hw_clks = {
		.hws = a9_mclk0_hw_clks,
		.num = ARRAY_SIZE(a9_mclk0_hw_clks),
	},
};

static const struct meson_clkc_data a9_mclk1_data = {
	.hw_clks = {
		.hws = a9_mclk1_hw_clks,
		.num = ARRAY_SIZE(a9_mclk1_hw_clks),
	},
};

static const struct of_device_id a9_pll_clkc_match_table[] = {
	{ .compatible = "amlogic,a9-gp0-pll",	.data = &a9_gp0_data, },
	{ .compatible = "amlogic,a9-hifi0-pll",	.data = &a9_hifi0_data, },
	{ .compatible = "amlogic,a9-hifi1-pll",	.data = &a9_hifi1_data, },
	{ .compatible = "amlogic,a9-mclk0-pll",	.data = &a9_mclk0_data, },
	{ .compatible = "amlogic,a9-mclk1-pll", .data = &a9_mclk1_data, },
	{}
};
MODULE_DEVICE_TABLE(of, a9_pll_clkc_match_table);

static struct platform_driver a9_pll_clkc_driver = {
	.probe		= meson_clkc_mmio_probe,
	.driver		= {
		.name	= "a9-pll-clkc",
		.of_match_table = a9_pll_clkc_match_table,
	},
};
module_platform_driver(a9_pll_clkc_driver);

MODULE_DESCRIPTION("Amlogic A9 PLL Clock Controller Driver");
MODULE_AUTHOR("Jian Hu <jian.hu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_MESON");
