// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Amlogic T7 PLL Clock Controller Driver
 *
 * Copyright (c) 2022-2023 Amlogic, inc. All rights reserved
 * Author: Yu Tu <yu.tu@amlogic.com>
 */
#include <linux/clk-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mpll.h"
#include "clk-pll.h"
#include "clk-regmap.h"
#include "t7-pll.h"
#include "meson-clkc-utils.h"
#include <dt-bindings/clock/amlogic,t7-pll-clkc.h>

static DEFINE_SPINLOCK(meson_clk_lock);

static struct clk_regmap t7_fixed_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_FIXPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_FIXPLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_FIXPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = ANACTRL_FIXPLL_CTRL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = ANACTRL_FIXPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_FIXPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
	},
	.hw.init = &(struct clk_init_data){
		.name = "fixed_pll_dco",
		.ops = &meson_clk_pll_ro_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL | CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_regmap t7_fixed_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_FIXPLL_CTRL0,
		.shift = 16,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fixed_pll",
		.ops = &clk_regmap_divider_ro_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fixed_pll_dco.hw
		},
		.num_parents = 1,
		/*
		 * This clock won't ever change at runtime so
		 * CLK_SET_RATE_PARENT is not required
		 */
		.flags = CLK_IS_CRITICAL | CLK_GET_RATE_NOCACHE,
	},
};

static const struct clk_ops meson_pll_clk_no_ops = {};

/*
 * the sys pll DCO value should be 3G~6G,
 * otherwise the sys pll can not lock.
 * od is for 32 bit.
 */

static const struct pll_params_table t7_sys_pll_params_table[] = {
	PLL_PARAMS(67, 1), /*DCO=1608M OD=1608MM*/
	PLL_PARAMS(71, 1), /*DCO=1704MM OD=1704M*/
	PLL_PARAMS(75, 1), /*DCO=1800M OD=1800M*/
	PLL_PARAMS(126, 1), /*DCO=3024 OD=1512M*/
	PLL_PARAMS(116, 1), /*DCO=2784 OD=1392M*/
	PLL_PARAMS(118, 1), /*DCO=2832M OD=1416M*/
	PLL_PARAMS(100, 1), /*DCO=2400M OD=1200M*/
	PLL_PARAMS(79, 1), /*DCO=1896M OD=1896M*/
	PLL_PARAMS(80, 1), /*DCO=1920M OD=1920M*/
	PLL_PARAMS(84, 1), /*DCO=2016M OD=2016M*/
	PLL_PARAMS(92, 1), /*DCO=2208M OD=2208M*/
	{0, 0},
};

static struct clk_regmap t7_sys_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_SYS0PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_SYS0PLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_SYS0PLL_CTRL0,
			.shift   = 16,
			.width   = 5,
		},
		.l = {
			.reg_off = ANACTRL_SYS0PLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_SYS0PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_sys_pll_params_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		/* This clock feeds the CPU, avoid disabling it */
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_regmap t7_sys1_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_SYS1PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_SYS1PLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_SYS1PLL_CTRL0,
			.shift   = 16,
			.width   = 5,
		},
		.table = t7_sys_pll_params_table,
		.l = {
			.reg_off = ANACTRL_SYS1PLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_SYS1PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys1_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		/* This clock feeds the CPU, avoid disabling it */
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_regmap t7_sys_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_SYS0PLL_CTRL0,
		.shift = 12,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sys_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sys1_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_SYS1PLL_CTRL0,
		.shift = 12,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sys1_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor t7_fclk_div2_div = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_fixed_pll.hw },
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div2_div.hw
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_fixed_factor t7_fclk_div3_div = {
	.mult = 1,
	.div = 3,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div3_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_fixed_pll.hw },
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 20,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div3",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div3_div.hw
		},
		.num_parents = 1,
		/*
		 * This clock is used by the resident firmware and is required
		 * by the platform to operate correctly.
		 * Until the following condition are met, we need this clock to
		 * be marked as critical:
		 * a) Mark the clock used by a firmware resource, if possible
		 * b) CCF has a clock hand-off mechanism to make the sure the
		 *    clock stays on until the proper driver comes along
		 */
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_fixed_factor t7_fclk_div4_div = {
	.mult = 1,
	.div = 4,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div4_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_fixed_pll.hw },
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div4 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 21,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div4",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div4_div.hw
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_fixed_factor t7_fclk_div5_div = {
	.mult = 1,
	.div = 5,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div5_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_fixed_pll.hw },
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div5 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 22,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div5",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div5_div.hw
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_fixed_factor t7_fclk_div7_div = {
	.mult = 1,
	.div = 7,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div7_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_fixed_pll.hw },
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div7 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 23,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div7",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div7_div.hw
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL,
	},
};

static struct clk_fixed_factor t7_fclk_div2p5_div = {
	.mult = 2,
	.div = 5,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2p5_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fixed_pll.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fclk_div2p5 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_FIXPLL_CTRL1,
		.bit_idx = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2p5",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fclk_div2p5_div.hw
		},
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL,
	},
};

static const struct pll_params_table t7_gp0_pll_table[] = {
	PLL_PARAMS(141, 1), /* DCO = 3384M OD = 2 PLL = 846M */
	PLL_PARAMS(132, 1), /* DCO = 3168M OD = 2 PLL = 792M */
	PLL_PARAMS(248, 1), /* DCO = 5952M OD = 3 PLL = 744M */
	PLL_PARAMS(96, 1), /* DCO = 2304M OD = 1 PLL = 1152M */
	{ /* sentinel */  }
};

/*
 * Internal gp0 pll emulation configuration parameters
 */
static const struct reg_sequence t7_gp0_init_regs[] = {
	{ .reg = ANACTRL_GP0PLL_CTRL1,	.def = 0x00000000 },
	{ .reg = ANACTRL_GP0PLL_CTRL2,	.def = 0x00000000 },
	{ .reg = ANACTRL_GP0PLL_CTRL3,	.def = 0x48681c00 },
	{ .reg = ANACTRL_GP0PLL_CTRL4,	.def = 0x88770290 },
	{ .reg = ANACTRL_GP0PLL_CTRL5,	.def = 0x3927200a },
	{ .reg = ANACTRL_GP0PLL_CTRL6,	.def = 0x56540000 }
};

static struct clk_regmap t7_gp0_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = ANACTRL_GP0PLL_CTRL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_gp0_pll_table,
		.init_regs = t7_gp0_init_regs,
		.init_count = ARRAY_SIZE(t7_gp0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_regmap t7_gp0_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_GP0PLL_CTRL0,
		.shift = 16,
		.width = 3,
		.flags = (CLK_DIVIDER_POWER_OF_TWO |
			  CLK_DIVIDER_ROUND_CLOSEST),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_gp0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT | CLK_GET_RATE_NOCACHE,
	},
};

static const struct pll_params_table t7_gp1_pll_table[] = {
	PLL_PARAMS(100, 1), /*DCO=4800M OD=1200M*/
	PLL_PARAMS(125, 1), /*DCO=3000M OD=1500M*/
	{ /* sentinel */  }
};

static const struct reg_sequence t7_gp1_init_regs[] = {
	{ .reg = ANACTRL_GP1PLL_CTRL1,	.def = 0x1420500f },
	{ .reg = ANACTRL_GP1PLL_CTRL2,	.def = 0x00023001 },
	{ .reg = ANACTRL_GP1PLL_CTRL3,	.def = 0x0, .delay_us = 20 },
};

static struct clk_regmap t7_gp1_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 16,
			.width   = 5,
		},
		.l = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_gp1_pll_table,
		.init_regs = t7_gp1_init_regs,
		.init_count = ARRAY_SIZE(t7_gp1_init_regs)
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp1_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		/* This clock feeds the DSU, avoid disabling it */
		.flags = CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_regmap t7_gp1_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_GP1PLL_CTRL0,
		.shift = 12,
		.width = 3,
		.flags = (CLK_DIVIDER_POWER_OF_TWO |
			  CLK_DIVIDER_ROUND_CLOSEST),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_gp1_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	},
};

static const struct pll_params_table t7_mclk_pll_table[] = {
	PLL_PARAMS(99, 1), /* DCO = 2376M OD = 1 PLL = 1152M */
	PLL_PARAMS(100, 1), /* DCO = 2400M */
	{ /* sentinel */  }
};

static const struct reg_sequence t7_mclk_init_regs[] = {
	{ .reg = ANACTRL_MCLK_PLL_CNTL0, .def = 0x20011064, .delay_us = 20 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL0, .def = 0x30011064 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL1, .def = 0x1470500f },
	{ .reg = ANACTRL_MCLK_PLL_CNTL2, .def = 0x00023041 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL3, .def = 0x18180000 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL4, .def = 0x00180303 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL0, .def = 0x10011064, .delay_us = 20 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL2, .def = 0x00023001, .delay_us = 20 }
};

static struct clk_regmap t7_mclk_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_MCLK_PLL_CNTL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_MCLK_PLL_CNTL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_MCLK_PLL_CNTL0,
			.shift   = 16,
			.width   = 5,
		},
		.l = {
			.reg_off = ANACTRL_MCLK_PLL_CNTL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_MCLK_PLL_CNTL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_mclk_pll_table,
		.init_regs = t7_mclk_init_regs,
		.init_count = ARRAY_SIZE(t7_mclk_init_regs),
		//.ignore_init = false
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED
	},
};

/* max div is 16 */
static const struct clk_div_table mclk_div[] = {
	{ .val = 0, .div = 1 },
	{ .val = 1, .div = 2 },
	{ .val = 2, .div = 4 },
	{ .val = 3, .div = 8 },
	{ .val = 4, .div = 16 },
	{ /* sentinel */ }
};

static struct clk_regmap t7_mclk_pre_od = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_MCLK_PLL_CNTL0,
		.shift = 12,
		.width = 3,
		.table = mclk_div,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_pre_od",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.shift = 16,
		.width = 5,
		.flags = CLK_DIVIDER_ONE_BASED | CLK_DIVIDER_ROUND_CLOSEST |
			 CLK_DIVIDER_ALLOW_ZERO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_pre_od.hw
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	},
};

static const struct pll_params_table t7_hifi_pll_table[] = {
	PLL_PARAMS(163, 1), /* DCO = 3932.16M */
	{ /* sentinel */  }
};

/*
 * Internal hifi pll emulation configuration parameters
 */
static const struct reg_sequence t7_hifi_init_regs[] = {
	{ .reg = ANACTRL_HIFIPLL_CTRL1,	.def = 0x00014820 }, /*frac = 20.16M */
	{ .reg = ANACTRL_HIFIPLL_CTRL2,	.def = 0x00000000 },
	{ .reg = ANACTRL_HIFIPLL_CTRL3,	.def = 0x6a285c00 },
	{ .reg = ANACTRL_HIFIPLL_CTRL4,	.def = 0x65771290 },
	{ .reg = ANACTRL_HIFIPLL_CTRL5,	.def = 0x3927200a },
	{ .reg = ANACTRL_HIFIPLL_CTRL6,	.def = 0x56540000 }
};

static struct clk_regmap t7_hifi_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = ANACTRL_HIFIPLL_CTRL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_hifi_pll_table,
		.init_regs = t7_hifi_init_regs,
		.init_count = ARRAY_SIZE(t7_hifi_init_regs),
		.flags = CLK_MESON_PLL_ROUND_CLOSEST,
		//.new_frac = 1,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hifi_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_hifi_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HIFIPLL_CTRL0,
		.shift = 16,
		.width = 2,
		.flags = (CLK_DIVIDER_POWER_OF_TWO |
			  CLK_DIVIDER_ROUND_CLOSEST),
	},
	.hw.init = &(struct clk_init_data){
		.name = "hifi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_hifi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT | CLK_GET_RATE_NOCACHE,
	},
};

/*
 * The Meson t7 PCIE PLL is fined tuned to deliver a very precise
 * 100MHz reference clock for the PCIe Analog PHY, and thus requires
 * a strict register sequence to enable the PLL.
 */
static const struct reg_sequence t7_pcie_pll_init_regs[] = {
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x200c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x300c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL1,	.def = 0x30000000 },
	{ .reg = ANACTRL_PCIEPLL_CTRL2,	.def = 0x00001100 },
	{ .reg = ANACTRL_PCIEPLL_CTRL3,	.def = 0x10058e00 },
	{ .reg = ANACTRL_PCIEPLL_CTRL4,	.def = 0x000100c0 },
	{ .reg = ANACTRL_PCIEPLL_CTRL5,	.def = 0x68000040 },
	{ .reg = ANACTRL_PCIEPLL_CTRL5,	.def = 0x68000060, .delay_us = 20 },
	{ .reg = ANACTRL_PCIEPLL_CTRL4,	.def = 0x008100c0, .delay_us = 10 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x340c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x140c04c8, .delay_us = 10 },
	{ .reg = ANACTRL_PCIEPLL_CTRL2,	.def = 0x00001000 }
};

/* Keep a single entry table for recalc/round_rate() ops */
static const struct pll_params_table t7_pcie_pll_table[] = {
	PLL_PARAMS(150, 1),
	{0, 0}
};

static struct clk_regmap t7_pcie_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_PCIEPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_PCIEPLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_PCIEPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = ANACTRL_PCIEPLL_CTRL1,
			.shift   = 0,
			.width   = 12,
		},
		.l = {
			.reg_off = ANACTRL_PCIEPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_PCIEPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = t7_pcie_pll_table,
		.init_regs = t7_pcie_pll_init_regs,
		.init_count = ARRAY_SIZE(t7_pcie_pll_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_dco",
		.ops = &meson_clk_pcie_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_fixed_factor t7_pcie_pll_dco_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_dco_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_pcie_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_pcie_pll_od = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_PCIEPLL_CTRL0,
		.shift = 16,
		.width = 5,
		.flags = CLK_DIVIDER_ROUND_CLOSEST |
			 CLK_DIVIDER_ONE_BASED |
			 CLK_DIVIDER_ALLOW_ZERO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_od",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_pcie_pll_dco_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT | CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_fixed_factor t7_pcie_pll = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_pcie_pll_od.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_pcie_bgp = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_PCIEPLL_CTRL5,
		.bit_idx = 27,
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_bgp",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_pcie_pll.hw },
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	},
};

static struct clk_regmap t7_pcie_hcsl = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_PCIEPLL_CTRL5,
		.bit_idx = 3,
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_hcsl",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_pcie_bgp.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT | CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_regmap t7_hdmi_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = ANACTRL_HDMIPLL_CTRL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll_dco",
		.ops = &meson_clk_pll_ro_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
		/*
		 * Display directly handle hdmi pll registers ATM, we need
		 * NOCACHE to keep our view of the clock as accurate as
		 * possible
		 */
		.flags = CLK_GET_RATE_NOCACHE,
	},
};

static struct clk_regmap t7_hdmi_pll_od = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HDMIPLL_CTRL0,
		.shift = 16,
		.width = 4,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll_od",
		.ops = &clk_regmap_divider_ro_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_hdmi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_hdmi_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HDMIPLL_CTRL0,
		.shift = 20,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll",
		.ops = &clk_regmap_divider_ro_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_hdmi_pll_od.hw
		},
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor t7_mpll_50m_div = {
	.mult = 1,
	.div = 80,
	.hw.init = &(struct clk_init_data){
		.name = "mpll_50m_div",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fixed_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static const struct clk_parent_data t7_mpll_50m_sel[] = {
	{ .fw_name = "xtal", },
	{ .hw = &t7_mpll_50m_div.hw },
};

static struct clk_regmap t7_mpll_50m = {
	.data = &(struct clk_regmap_mux_data){
		.offset = ANACTRL_FIXPLL_CTRL3,
		.mask = 0x1,
		.shift = 5,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll_50m",
		.ops = &clk_regmap_mux_ro_ops,
		.parent_data = t7_mpll_50m_sel,
		.num_parents = ARRAY_SIZE(t7_mpll_50m_sel),
	},
};

static struct clk_fixed_factor t7_mpll_prediv = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mpll_prediv",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fixed_pll_dco.hw
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence t7_mpll0_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL2, .def = 0x40000033 }
};

static struct clk_regmap t7_mpll0_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = ANACTRL_MPLL_CTRL1,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = ANACTRL_MPLL_CTRL1,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = ANACTRL_MPLL_CTRL1,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = ANACTRL_MPLL_CTRL1,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
		.init_regs = t7_mpll0_init_regs,
		.init_count = ARRAY_SIZE(t7_mpll0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_mpll0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL1,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mpll0_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence t7_mpll1_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL4,	.def = 0x40000033 }
};

static struct clk_regmap t7_mpll1_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = ANACTRL_MPLL_CTRL3,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = ANACTRL_MPLL_CTRL3,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = ANACTRL_MPLL_CTRL3,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = ANACTRL_MPLL_CTRL3,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
		.init_regs = t7_mpll1_init_regs,
		.init_count = ARRAY_SIZE(t7_mpll1_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_mpll1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL3,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mpll1_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence t7_mpll2_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL6, .def = 0x40000033 }
};

static struct clk_regmap t7_mpll2_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = ANACTRL_MPLL_CTRL5,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = ANACTRL_MPLL_CTRL5,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = ANACTRL_MPLL_CTRL5,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = ANACTRL_MPLL_CTRL5,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
		.init_regs = t7_mpll2_init_regs,
		.init_count = ARRAY_SIZE(t7_mpll2_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_mpll2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL5,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mpll2_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence t7_mpll3_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL8, .def = 0x40000033 }
};

static struct clk_regmap t7_mpll3_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = ANACTRL_MPLL_CTRL7,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = ANACTRL_MPLL_CTRL7,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = ANACTRL_MPLL_CTRL7,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = ANACTRL_MPLL_CTRL7,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
		.init_regs = t7_mpll3_init_regs,
		.init_count = ARRAY_SIZE(t7_mpll3_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_mpll3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL7,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mpll3_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.mask = 0x3,
		.shift = 4,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .hw = &t7_mclk_pll.hw },
			{ .fw_name = "xtal", },
			{ .hw = &t7_mpll_50m.hw },
		},
		.num_parents = 3,
	},
};

static struct clk_fixed_factor t7_mclk_0_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk_0_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mclk_0_sel.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_0_pre = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 2,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_0_pre",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_0_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 0,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_0_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.mask = 0x3,
		.shift = 12,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .hw = &t7_mclk_pll.hw },
			{ .fw_name = "xtal", },
			{ .hw = &t7_mpll_50m.hw },
		},
		.num_parents = 3,
	},
};

static struct clk_fixed_factor t7_mclk_1_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk_1_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_mclk_1_sel.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_1_pre = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 10,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_1_pre",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_1_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mclk_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mclk_1_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/* Array of all clocks provided by this provider */
static struct clk_hw *t7_pll_hw_clks[] = {
	[CLKID_FIXED_PLL_DCO]		= &t7_fixed_pll_dco.hw,
	[CLKID_FIXED_PLL]		= &t7_fixed_pll.hw,
	[CLKID_SYS_PLL_DCO]		= &t7_sys_pll_dco.hw,
	[CLKID_SYS_PLL]			= &t7_sys_pll.hw,
	[CLKID_SYS1_PLL_DCO]		= &t7_sys1_pll_dco.hw,
	[CLKID_SYS1_PLL]		= &t7_sys1_pll.hw,
	[CLKID_FCLK_DIV2_DIV]		= &t7_fclk_div2_div.hw,
	[CLKID_FCLK_DIV2]		= &t7_fclk_div2.hw,
	[CLKID_FCLK_DIV3_DIV]		= &t7_fclk_div3_div.hw,
	[CLKID_FCLK_DIV3]		= &t7_fclk_div3.hw,
	[CLKID_FCLK_DIV4_DIV]		= &t7_fclk_div4_div.hw,
	[CLKID_FCLK_DIV4]		= &t7_fclk_div4.hw,
	[CLKID_FCLK_DIV5_DIV]		= &t7_fclk_div5_div.hw,
	[CLKID_FCLK_DIV5]		= &t7_fclk_div5.hw,
	[CLKID_FCLK_DIV7_DIV]		= &t7_fclk_div7_div.hw,
	[CLKID_FCLK_DIV7]		= &t7_fclk_div7.hw,
	[CLKID_FCLK_DIV2P5_DIV]		= &t7_fclk_div2p5_div.hw,
	[CLKID_FCLK_DIV2P5]		= &t7_fclk_div2p5.hw,
	[CLKID_GP0_PLL_DCO]		= &t7_gp0_pll_dco.hw,
	[CLKID_GP0_PLL]			= &t7_gp0_pll.hw,
	[CLKID_GP1_PLL_DCO]		= &t7_gp1_pll_dco.hw,
	[CLKID_GP1_PLL]			= &t7_gp1_pll.hw,
	[CLKID_MCLK_PLL_DCO]		= &t7_mclk_pll_dco.hw,
	[CLKID_MCLK_PRE]		= &t7_mclk_pre_od.hw,
	[CLKID_MCLK_PLL]		= &t7_mclk_pll.hw,
	[CLKID_HIFI_PLL_DCO]		= &t7_hifi_pll_dco.hw,
	[CLKID_HIFI_PLL]		= &t7_hifi_pll.hw,
	[CLKID_PCIE_PLL_DCO]		= &t7_pcie_pll_dco.hw,
	[CLKID_PCIE_PLL_DCO_DIV2]	= &t7_pcie_pll_dco_div2.hw,
	[CLKID_PCIE_PLL_OD]		= &t7_pcie_pll_od.hw,
	[CLKID_PCIE_PLL]		= &t7_pcie_pll.hw,
	[CLKID_PCIE_BGP]		= &t7_pcie_bgp.hw,
	[CLKID_PCIE_HCSL]		= &t7_pcie_hcsl.hw,
	[CLKID_HDMI_PLL_DCO]		= &t7_hdmi_pll_dco.hw,
	[CLKID_HDMI_PLL_OD]		= &t7_hdmi_pll_od.hw,
	[CLKID_HDMI_PLL]		= &t7_hdmi_pll.hw,
	[CLKID_MPLL_50M_DIV]		= &t7_mpll_50m_div.hw,
	[CLKID_MPLL_50M]		= &t7_mpll_50m.hw,
	[CLKID_MPLL_PREDIV]		= &t7_mpll_prediv.hw,
	[CLKID_MPLL0_DIV]		= &t7_mpll0_div.hw,
	[CLKID_MPLL0]			= &t7_mpll0.hw,
	[CLKID_MPLL1_DIV]		= &t7_mpll1_div.hw,
	[CLKID_MPLL1]			= &t7_mpll1.hw,
	[CLKID_MPLL2_DIV]		= &t7_mpll2_div.hw,
	[CLKID_MPLL2]			= &t7_mpll2.hw,
	[CLKID_MPLL3_DIV]		= &t7_mpll3_div.hw,
	[CLKID_MPLL3]			= &t7_mpll3.hw,
	[CLKID_MCLK_0_SEL]		= &t7_mclk_0_sel.hw,
	[CLKID_MCLK_0_DIV2]		= &t7_mclk_0_div2.hw,
	[CLKID_MCLK_0_PRE]		= &t7_mclk_0_pre.hw,
	[CLKID_MCLK_0]			= &t7_mclk_0.hw,
	[CLKID_MCLK_1_SEL]		= &t7_mclk_1_sel.hw,
	[CLKID_MCLK_1_DIV2]		= &t7_mclk_1_div2.hw,
	[CLKID_MCLK_1_PRE]		= &t7_mclk_1_pre.hw,
	[CLKID_MCLK_1]			= &t7_mclk_1.hw,
};

static struct clk_regmap *const t7_pll_clk_regmaps[] = {
	&t7_fixed_pll_dco,
	&t7_fixed_pll,
	&t7_sys_pll_dco,
	&t7_sys_pll,
	&t7_sys1_pll_dco,
	&t7_sys1_pll,
	&t7_fclk_div2,
	&t7_fclk_div3,
	&t7_fclk_div4,
	&t7_fclk_div5,
	&t7_fclk_div7,
	&t7_fclk_div2p5,
	&t7_gp0_pll_dco,
	&t7_gp0_pll,
	&t7_gp1_pll_dco,
	&t7_gp1_pll,
	&t7_mclk_pll_dco,
	&t7_mclk_pre_od,
	&t7_mclk_pll,
	&t7_hifi_pll_dco,
	&t7_hifi_pll,
	&t7_pcie_pll_dco,
	&t7_pcie_pll_od,
	&t7_pcie_bgp,
	&t7_pcie_hcsl,
	&t7_hdmi_pll_dco,
	&t7_hdmi_pll_od,
	&t7_hdmi_pll,
	&t7_mpll_50m,
	&t7_mpll0_div,
	&t7_mpll0,
	&t7_mpll1_div,
	&t7_mpll1,
	&t7_mpll2_div,
	&t7_mpll2,
	&t7_mpll3_div,
	&t7_mpll3,
	&t7_mclk_0_sel,
	&t7_mclk_0_pre,
	&t7_mclk_0,
	&t7_mclk_1_sel,
	&t7_mclk_1_pre,
	&t7_mclk_1
};

static const struct reg_sequence t7_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL0,	.def = 0x00000543 },
};

static struct regmap_config clkc_regmap_config = {
	.reg_bits       = 32,
	.val_bits       = 32,
	.reg_stride     = 4,
};

static struct meson_clk_hw_data t7_pll_clks = {
	.hws = t7_pll_hw_clks,
	.num = ARRAY_SIZE(t7_pll_hw_clks),
};

static int amlogic_t7_pll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	void __iomem *base;
	int ret, i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return dev_err_probe(dev, PTR_ERR(base), "can't ioremap resource\n");

	regmap = devm_regmap_init_mmio(dev, base, &clkc_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "can't init regmap mmio region\n");

	ret = regmap_multi_reg_write(regmap, t7_init_regs, ARRAY_SIZE(t7_init_regs));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to init registers\n");

	for (i = 0; i < ARRAY_SIZE(t7_pll_clk_regmaps); i++)
		t7_pll_clk_regmaps[i]->map = regmap;

	/* Register clocks */
	for (i = 0; i < t7_pll_clks.num; i++) {
		/* array might be sparse */
		if (!t7_pll_clks.hws[i])
			continue;

		ret = devm_clk_hw_register(dev, t7_pll_clks.hws[i]);
		if (ret)
			return dev_err_probe(dev, ret, "clock[%d] registration failed\n", i);
	}

	return devm_of_clk_add_hw_provider(dev, meson_clk_hw_get, &t7_pll_clks);
}

static const struct of_device_id t7_pll_clkc_match_table[] = {
	{ .compatible = "amlogic,t7-pll-clkc", },
	{}
};
MODULE_DEVICE_TABLE(of, t7_pll_clkc_match_table);

static struct platform_driver t7_pll_clkc_driver = {
	.probe		= amlogic_t7_pll_probe,
	.driver		= {
		.name	= "t7-pll-clkc",
		.of_match_table = t7_pll_clkc_match_table,
	},
};

module_platform_driver(t7_pll_clkc_driver);
MODULE_AUTHOR("Yu Tu <yu.tu@amlogic.com>");
MODULE_AUTHOR("Lucas Tanure <tanure@linux.com>");
MODULE_LICENSE("GPL");
