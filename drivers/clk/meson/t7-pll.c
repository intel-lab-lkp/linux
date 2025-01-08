// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2024 Amlogic, Inc. All rights reserved.
 * Author: Jian Hu <jian.hu@amlogic.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include "clk-regmap.h"
#include "clk-pll.h"
#include "clk-mpll.h"
#include "meson-clkc-utils.h"
#include "meson-eeclk.h"
#include <dt-bindings/clock/amlogic,t7-pll-clkc.h>

#define ANACTRL_GP0PLL_CTRL0		0x00
#define ANACTRL_GP0PLL_CTRL1		0x04
#define ANACTRL_GP0PLL_CTRL2		0x08
#define ANACTRL_GP0PLL_CTRL3		0x0c
#define ANACTRL_GP0PLL_CTRL4		0x10
#define ANACTRL_GP0PLL_CTRL5		0x14
#define ANACTRL_GP0PLL_CTRL6		0x18
#define ANACTRL_GP0PLL_STS		0x1c

#define ANACTRL_GP1PLL_CTRL0		0x00
#define ANACTRL_GP1PLL_CTRL1		0x04
#define ANACTRL_GP1PLL_CTRL2		0x08
#define ANACTRL_GP1PLL_CTRL3		0x0c
#define ANACTRL_GP1PLL_STS		0x1c

#define ANACTRL_HIFIPLL_CTRL0		0x00
#define ANACTRL_HIFIPLL_CTRL1		0x04
#define ANACTRL_HIFIPLL_CTRL2		0x08
#define ANACTRL_HIFIPLL_CTRL3		0x0c
#define ANACTRL_HIFIPLL_CTRL4		0x10
#define ANACTRL_HIFIPLL_CTRL5		0x14
#define ANACTRL_HIFIPLL_CTRL6		0x18
#define ANACTRL_HIFIPLL_STS		0x1c

#define ANACTRL_PCIEPLL_CTRL0		0x00
#define ANACTRL_PCIEPLL_CTRL1		0x04
#define ANACTRL_PCIEPLL_CTRL2		0x08
#define ANACTRL_PCIEPLL_CTRL3		0x0c
#define ANACTRL_PCIEPLL_CTRL4		0x10
#define ANACTRL_PCIEPLL_CTRL5		0x14
#define ANACTRL_PCIEPLL_STS		0x18

#define ANACTRL_MPLL_CTRL0		0x00
#define ANACTRL_MPLL_CTRL1		0x04
#define ANACTRL_MPLL_CTRL2		0x08
#define ANACTRL_MPLL_CTRL3		0x0c
#define ANACTRL_MPLL_CTRL4		0x10
#define ANACTRL_MPLL_CTRL5		0x14
#define ANACTRL_MPLL_CTRL6		0x18
#define ANACTRL_MPLL_CTRL7		0x1c
#define ANACTRL_MPLL_CTRL8		0x20
#define ANACTRL_MPLL_STS		0x24

#define ANACTRL_HDMIPLL_CTRL0		0x00
#define ANACTRL_HDMIPLL_CTRL1		0x04
#define ANACTRL_HDMIPLL_CTRL2		0x08
#define ANACTRL_HDMIPLL_CTRL3		0x0c
#define ANACTRL_HDMIPLL_CTRL4		0x10
#define ANACTRL_HDMIPLL_CTRL5		0x14
#define ANACTRL_HDMIPLL_CTRL6		0x18
#define ANACTRL_HDMIPLL_STS		0x1c

#define ANACTRL_MCLK_PLL_CNTL0		0x00
#define ANACTRL_MCLK_PLL_CNTL1		0x04
#define ANACTRL_MCLK_PLL_CNTL2		0x08
#define ANACTRL_MCLK_PLL_CNTL3		0x0c
#define ANACTRL_MCLK_PLL_CNTL4		0x10
#define ANACTRL_MCLK_PLL_STS		0x14

static const struct pll_mult_range media_pll_mult_range = {
	.min = 125,
	.max = 250,
};

static const struct reg_sequence gp0_init_regs[] = {
	{ .reg = ANACTRL_GP0PLL_CTRL1,  .def = 0x00000000 },
	{ .reg = ANACTRL_GP0PLL_CTRL2,  .def = 0x00000000 },
	{ .reg = ANACTRL_GP0PLL_CTRL3,  .def = 0x48681c00 },
	{ .reg = ANACTRL_GP0PLL_CTRL4,  .def = 0x88770290 },
	{ .reg = ANACTRL_GP0PLL_CTRL5,  .def = 0x3927200a },
	{ .reg = ANACTRL_GP0PLL_CTRL6,  .def = 0x56540000 },
};

static struct clk_regmap gp0_pll_dco = {
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
		.l = {
			.reg_off = ANACTRL_GP0PLL_STS,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_GP0PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.range = &media_pll_mult_range,
		.init_regs = gp0_init_regs,
		.init_count = ARRAY_SIZE(gp0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap gp0_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_GP0PLL_CTRL0,
		.shift = 16,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&gp0_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The gp1 pll IP is different with gp0 pll, the PLL DCO range is
 * 1.6GHZ - 3.2GHZ, and the reg_sequence is short
 */
static const struct pll_mult_range gp1_pll_mult_range = {
	.min = 67,
	.max = 133,
};

static const struct reg_sequence gp1_init_regs[] = {
	{ .reg = ANACTRL_GP1PLL_CTRL1,  .def = 0x1420500f },
	{ .reg = ANACTRL_GP1PLL_CTRL2,  .def = 0x00023001 },
	{ .reg = ANACTRL_GP1PLL_CTRL3,  .def = 0x00000000 },
};

static struct clk_regmap gp1_pll_dco = {
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
			.reg_off = ANACTRL_GP1PLL_STS,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_GP1PLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.range = &gp1_pll_mult_range,
		.init_regs = gp1_init_regs,
		.init_count = ARRAY_SIZE(gp1_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp1_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap gp1_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_GP1PLL_CTRL0,
		.shift = 12,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "gp1_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&gp1_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence hifi_init_regs[] = {
	{ .reg = ANACTRL_HIFIPLL_CTRL1, .def = 0x00000000 },
	{ .reg = ANACTRL_HIFIPLL_CTRL2, .def = 0x00000000 },
	{ .reg = ANACTRL_HIFIPLL_CTRL3, .def = 0x6a285c00 },
	{ .reg = ANACTRL_HIFIPLL_CTRL4, .def = 0x65771290 },
	{ .reg = ANACTRL_HIFIPLL_CTRL5, .def = 0x3927200a },
	{ .reg = ANACTRL_HIFIPLL_CTRL6, .def = 0x56540000 }
};

static struct clk_regmap hifi_pll_dco = {
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
		.l = {
			.reg_off = ANACTRL_HIFIPLL_STS,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = ANACTRL_HIFIPLL_CTRL0,
			.shift   = 29,
			.width   = 1,
		},
		.range = &media_pll_mult_range,
		.init_regs = hifi_init_regs,
		.init_count = ARRAY_SIZE(hifi_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "hifi_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap hifi_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HIFIPLL_CTRL0,
		.shift = 16,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "hifi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&hifi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The T7 PCIE PLL is fined tuned to deliver a very precise
 * 100MHz reference clock for the PCIe Analog PHY, and thus requires
 * a strict register sequence to enable the PLL.
 */
static const struct reg_sequence pcie_pll_init_regs[] = {
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x200c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x300c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL1,	.def = 0x30000000 },
	{ .reg = ANACTRL_PCIEPLL_CTRL2,	.def = 0x00001100 },
	{ .reg = ANACTRL_PCIEPLL_CTRL3,	.def = 0x10058e00 },
	{ .reg = ANACTRL_PCIEPLL_CTRL4,	.def = 0x000100c0 },
	{ .reg = ANACTRL_PCIEPLL_CTRL5,	.def = 0x68000048 },
	{ .reg = ANACTRL_PCIEPLL_CTRL5,	.def = 0x68000068, .delay_us = 20 },
	{ .reg = ANACTRL_PCIEPLL_CTRL4,	.def = 0x008100c0, .delay_us = 20 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x340c04c8 },
	{ .reg = ANACTRL_PCIEPLL_CTRL0,	.def = 0x140c04c8, .delay_us = 20 },
	{ .reg = ANACTRL_PCIEPLL_CTRL2,	.def = 0x00001000 }
};

static struct clk_regmap pcie_pll_dco = {
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
		.init_regs = pcie_pll_init_regs,
		.init_count = ARRAY_SIZE(pcie_pll_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_dco",
		.ops = &meson_clk_pcie_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
	},
};

static struct clk_fixed_factor pcie_pll_dco_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_dco_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&pcie_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap pcie_pll_od = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_PCIEPLL_CTRL0,
		.shift = 16,
		.width = 5,
		.flags = CLK_DIVIDER_ONE_BASED |
			 CLK_DIVIDER_ALLOW_ZERO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll_od",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&pcie_pll_dco_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor pcie_pll = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "pcie_pll",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&pcie_pll_od.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor mpll_prediv = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mpll_prediv",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
	},
};

static const struct reg_sequence mpll0_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL2, .def = 0x40000033 }
};

static struct clk_regmap mpll0_div = {
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
		.init_regs = mpll0_init_regs,
		.init_count = ARRAY_SIZE(mpll0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap mpll0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL1,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &mpll0_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence mpll1_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL4,	.def = 0x40000033 }
};

static struct clk_regmap mpll1_div = {
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
		.init_regs = mpll1_init_regs,
		.init_count = ARRAY_SIZE(mpll1_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap mpll1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL3,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &mpll1_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence mpll2_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL6, .def = 0x40000033 }
};

static struct clk_regmap mpll2_div = {
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
		.init_regs = mpll2_init_regs,
		.init_count = ARRAY_SIZE(mpll2_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap mpll2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL5,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &mpll2_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence mpll3_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL8, .def = 0x40000033 }
};

static struct clk_regmap mpll3_div = {
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
		.init_regs = mpll3_init_regs,
		.init_count = ARRAY_SIZE(mpll3_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3_div",
		.ops = &meson_clk_mpll_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mpll_prediv.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap mpll3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MPLL_CTRL7,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &mpll3_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct reg_sequence hdmi_init_regs[] = {
	{ .reg = ANACTRL_HDMIPLL_CTRL1, .def = 0x00000000 },
	{ .reg = ANACTRL_HDMIPLL_CTRL2, .def = 0x00000000 },
	{ .reg = ANACTRL_HDMIPLL_CTRL3, .def = 0x6a28dc00 },
	{ .reg = ANACTRL_HDMIPLL_CTRL4, .def = 0x65771290 },
	{ .reg = ANACTRL_HDMIPLL_CTRL5, .def = 0x39272000 },
	{ .reg = ANACTRL_HDMIPLL_CTRL6, .def = 0x56540000 }
};

static struct clk_regmap hdmi_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 0,
			.width   = 9,
		},
		.n = {
			.reg_off = ANACTRL_HDMIPLL_CTRL0,
			.shift   = 10,
			.width   = 5,
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
		.range = &media_pll_mult_range,
		.init_regs = hdmi_init_regs,
		.init_count = ARRAY_SIZE(hdmi_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "input", }
		},
		.num_parents = 1,
	},
};

static struct clk_regmap hdmi_pll_od = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HDMIPLL_CTRL0,
		.shift = 16,
		.width = 4,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll_od",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&hdmi_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap hdmi_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_HDMIPLL_CTRL0,
		.shift = 20,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hdmi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&hdmi_pll_od.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct pll_mult_range mclk_pll_mult_range = {
	.min = 67,
	.max = 133,
};

static const struct reg_sequence mclk_init_regs[] = {
	{ .reg = ANACTRL_MCLK_PLL_CNTL1, .def = 0x1470500f },
	{ .reg = ANACTRL_MCLK_PLL_CNTL2, .def = 0x00023041 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL3, .def = 0x18180000 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL4, .def = 0x00180303 },
	{ .reg = ANACTRL_MCLK_PLL_CNTL2, .def = 0x00023001, .delay_us = 20 }
};

static struct clk_regmap mclk_pll_dco = {
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
		.range = &mclk_pll_mult_range,
		.init_regs = mclk_init_regs,
		.init_count = ARRAY_SIZE(mclk_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "input",
		},
		.num_parents = 1,
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

static struct clk_regmap mclk_pre_od = {
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
			&mclk_pll_dco.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.shift = 16,
		.width = 5,
		.flags = CLK_DIVIDER_ONE_BASED,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mclk_pre_od.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.mask = 0x3,
		.shift = 4,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .hw = &mclk_pll.hw },
			{ .fw_name = "mclk_in0", },
			{ .fw_name = "mclk_in1", },
		},
		.num_parents = 3,
	},
};

static struct clk_fixed_factor mclk_0_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk_0_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &mclk_0_sel.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_0_pre = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 2,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_0_pre",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mclk_0_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 0,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mclk_0_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.mask = 0x3,
		.shift = 12,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mclk_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .hw = &mclk_pll.hw },
			{ .fw_name = "mclk_in0", },
			{ .fw_name = "mclk_in1", },
		},
		.num_parents = 3,
	},
};

static struct clk_fixed_factor mclk_1_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "mclk_1_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) { &mclk_1_sel.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_1_pre = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 10,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_1_pre",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mclk_1_div2.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mclk_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = ANACTRL_MCLK_PLL_CNTL4,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mclk_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mclk_1_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_hw *t7_gp0_hw_clks[] = {
	[CLKID_GP0_PLL_DCO]		= &gp0_pll_dco.hw,
	[CLKID_GP0_PLL]			= &gp0_pll.hw,
};

static struct clk_hw *t7_gp1_hw_clks[] = {
	[CLKID_GP1_PLL_DCO]		= &gp1_pll_dco.hw,
	[CLKID_GP0_PLL]			= &gp1_pll.hw,
};

static struct clk_hw *t7_hifi_hw_clks[] = {
	[CLKID_HIFI_PLL_DCO]		= &hifi_pll_dco.hw,
	[CLKID_HIFI_PLL]		= &hifi_pll.hw,
};

static struct clk_hw *t7_pcie_hw_clks[] = {
	[CLKID_PCIE_PLL_DCO]		= &pcie_pll_dco.hw,
	[CLKID_PCIE_PLL_DCO_DIV2]	= &pcie_pll_dco_div2.hw,
	[CLKID_PCIE_PLL_OD]		= &pcie_pll_od.hw,
	[CLKID_PCIE_PLL]		= &pcie_pll.hw,
};

static struct clk_hw *t7_mpll_hw_clks[] = {
	[CLKID_MPLL_PREDIV]		= &mpll_prediv.hw,
	[CLKID_MPLL0_DIV]		= &mpll0_div.hw,
	[CLKID_MPLL0]			= &mpll0.hw,
	[CLKID_MPLL1_DIV]		= &mpll1_div.hw,
	[CLKID_MPLL1]			= &mpll1.hw,
	[CLKID_MPLL2_DIV]		= &mpll2_div.hw,
	[CLKID_MPLL2]			= &mpll2.hw,
	[CLKID_MPLL3_DIV]		= &mpll3_div.hw,
	[CLKID_MPLL3]			= &mpll3.hw,
};

static struct clk_hw *t7_hdmi_hw_clks[] = {
	[CLKID_HDMI_PLL_DCO]		= &hdmi_pll_dco.hw,
	[CLKID_HDMI_PLL_OD]		= &hdmi_pll_od.hw,
	[CLKID_HDMI_PLL]		= &hdmi_pll.hw,
};

static struct clk_hw *t7_mclk_hw_clks[] = {
	[CLKID_MCLK_PLL_DCO]		= &mclk_pll_dco.hw,
	[CLKID_MCLK_PRE]		= &mclk_pre_od.hw,
	[CLKID_MCLK_PLL]		= &mclk_pll.hw,
	[CLKID_MCLK_0_SEL]		= &mclk_0_sel.hw,
	[CLKID_MCLK_0_DIV2]		= &mclk_0_div2.hw,
	[CLKID_MCLK_0_PRE]		= &mclk_0_pre.hw,
	[CLKID_MCLK_0]			= &mclk_0.hw,
	[CLKID_MCLK_1_SEL]		= &mclk_1_sel.hw,
	[CLKID_MCLK_1_DIV2]		= &mclk_1_div2.hw,
	[CLKID_MCLK_1_PRE]		= &mclk_1_pre.hw,
	[CLKID_MCLK_1]			= &mclk_1.hw,
};

static struct clk_regmap *const t7_gp0_regmaps[] = {
	&gp0_pll_dco,
	&gp0_pll,
};

static struct clk_regmap *const t7_gp1_regmaps[] = {
	&gp1_pll_dco,
	&gp1_pll,
};

static struct clk_regmap *const t7_hifi_regmaps[] = {
	&hifi_pll_dco,
	&hifi_pll,
};

static struct clk_regmap *const t7_pcie_regmaps[] = {
	&pcie_pll_dco,
	&pcie_pll_od,
};

static struct clk_regmap *const t7_mpll_regmaps[] = {
	&mpll0_div,
	&mpll0,
	&mpll1_div,
	&mpll1,
	&mpll2_div,
	&mpll2,
	&mpll3_div,
	&mpll3,
};

static struct clk_regmap *const t7_hdmi_regmaps[] = {
	&hdmi_pll_dco,
	&hdmi_pll_od,
	&hdmi_pll,
};

static struct clk_regmap *const t7_mclk_regmaps[] = {
	&mclk_pll_dco,
	&mclk_pre_od,
	&mclk_pll,
	&mclk_0_sel,
	&mclk_0_pre,
	&mclk_0,
	&mclk_1_sel,
	&mclk_1_pre,
	&mclk_1,
};

static const struct regmap_config clkc_regmap_config = {
	.reg_bits       = 32,
	.val_bits       = 32,
	.reg_stride     = 4,
};

static int amlogic_t7_pll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct meson_eeclkc_data *data;
	void __iomem *base;
	struct regmap *map;
	int i, ret;

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return -EINVAL;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	map = devm_regmap_init_mmio(dev, base, &clkc_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/* Populate regmap for the regmap backed clocks */
	for (i = 0; i < data->regmap_clk_num; i++)
		data->regmap_clks[i]->map = map;

	if (data->init_count)
		regmap_multi_reg_write(map, data->init_regs,
				       data->init_count);

	/* Register clocks */
	for (i = 0; i < data->hw_clks.num; i++) {
		ret = devm_clk_hw_register(dev, data->hw_clks.hws[i]);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(dev, meson_clk_hw_get, (void *)&data->hw_clks);
}

static const struct meson_eeclkc_data t7_gp0_data = {
	.regmap_clks = t7_gp0_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_gp0_regmaps),
	.hw_clks = {
		.hws = t7_gp0_hw_clks,
		.num = ARRAY_SIZE(t7_gp0_hw_clks),
	},
};

static const struct meson_eeclkc_data t7_gp1_data = {
	.regmap_clks = t7_gp1_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_gp1_regmaps),
	.hw_clks = {
		.hws = t7_gp1_hw_clks,
		.num = ARRAY_SIZE(t7_gp1_hw_clks),
	},
};

static const struct meson_eeclkc_data t7_hifi_data = {
	.regmap_clks = t7_hifi_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_hifi_regmaps),
	.hw_clks = {
		.hws = t7_hifi_hw_clks,
		.num = ARRAY_SIZE(t7_hifi_hw_clks),
	},
};

static const struct meson_eeclkc_data t7_pcie_data = {
	.regmap_clks = t7_pcie_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_pcie_regmaps),
	.hw_clks = {
		.hws = t7_pcie_hw_clks,
		.num = ARRAY_SIZE(t7_pcie_hw_clks),
	},
};

static const struct reg_sequence mpll_init_regs[] = {
	{ .reg = ANACTRL_MPLL_CTRL0, .def = 0x00000543 }
};

static const struct meson_eeclkc_data t7_mpll_data = {
	.regmap_clks = t7_mpll_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_mpll_regmaps),
	.init_regs = mpll_init_regs,
	.init_count = ARRAY_SIZE(mpll_init_regs),
	.hw_clks = {
		.hws = t7_mpll_hw_clks,
		.num = ARRAY_SIZE(t7_mpll_hw_clks),
	},
};

static const struct meson_eeclkc_data t7_hdmi_data = {
	.regmap_clks = t7_hdmi_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_hdmi_regmaps),
	.hw_clks = {
		.hws = t7_hdmi_hw_clks,
		.num = ARRAY_SIZE(t7_hdmi_hw_clks),
	},
};

static const struct meson_eeclkc_data t7_mclk_data = {
	.regmap_clks = t7_mclk_regmaps,
	.regmap_clk_num = ARRAY_SIZE(t7_mclk_regmaps),
	.hw_clks = {
		.hws = t7_mclk_hw_clks,
		.num = ARRAY_SIZE(t7_mclk_hw_clks),
	},
};

static const struct of_device_id t7_pll_clkc_match_table[] = {
	{
		.compatible = "amlogic,t7-pll-gp0",
		.data = &t7_gp0_data,
	},
	{
		.compatible = "amlogic,t7-pll-gp1",
		.data = &t7_gp1_data,
	},
	{
		.compatible = "amlogic,t7-pll-hifi",
		.data = &t7_hifi_data,
	},
	{
		.compatible = "amlogic,t7-pll-pcie",
		.data = &t7_pcie_data,
	},
	{
		.compatible = "amlogic,t7-mpll",
		.data = &t7_mpll_data,
	},
	{
		.compatible = "amlogic,t7-pll-hdmi",
		.data = &t7_hdmi_data,
	},
	{
		.compatible = "amlogic,t7-pll-mclk",
		.data = &t7_mclk_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, t7_pll_clkc_match_table);

static struct platform_driver t7_pll_clkc_driver = {
	.probe = amlogic_t7_pll_probe,
	.driver = {
		.name = "t7-pll-clkc",
		.of_match_table = t7_pll_clkc_match_table,
	},
};

MODULE_DESCRIPTION("Amlogic T7 PLL Clock Controller driver");
module_platform_driver(t7_pll_clkc_driver);
MODULE_AUTHOR("Jian Hu <jian.hu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CLK_MESON);
