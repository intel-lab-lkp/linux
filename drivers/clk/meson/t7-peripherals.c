// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2024 Amlogic, Inc. All rights reserved.
 * Author: Jian Hu <jian.hu@amlogic.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include "clk-dualdiv.h"
#include "clk-regmap.h"
#include "meson-clkc-utils.h"
#include <dt-bindings/clock/amlogic,t7-peripherals-clkc.h>

#define CLKCTRL_RTC_BY_OSCIN_CTRL0	0x8
#define CLKCTRL_RTC_BY_OSCIN_CTRL1	0xc
#define CLKCTRL_RTC_CTRL		0x10
#define CLKCTRL_SYS_CLK_CTRL0		0x40
#define CLKCTRL_SYS_CLK_EN0_REG0	0x44
#define CLKCTRL_SYS_CLK_EN0_REG1	0x48
#define CLKCTRL_SYS_CLK_EN0_REG2	0x4c
#define CLKCTRL_SYS_CLK_EN0_REG3	0x50
#define CLKCTRL_CECA_CTRL0		0x88
#define CLKCTRL_CECA_CTRL1		0x8c
#define CLKCTRL_CECB_CTRL0		0x90
#define CLKCTRL_CECB_CTRL1		0x94
#define CLKCTRL_SC_CLK_CTRL		0x98
#define CLKCTRL_DSPA_CLK_CTRL0		0x9c
#define CLKCTRL_DSPB_CLK_CTRL0		0xa0
#define CLKCTRL_CLK12_24_CTRL		0xa8
#define CLKCTRL_ANAKIN_CLK_CTRL		0xac
#define CLKCTRL_MIPI_CSI_PHY_CLK_CTRL	0x10c
#define CLKCTRL_MIPI_ISP_CLK_CTRL	0x110
#define CLKCTRL_TS_CLK_CTRL		0x158
#define CLKCTRL_MALI_CLK_CTRL		0x15c
#define CLKCTRL_ETH_CLK_CTRL		0x164
#define CLKCTRL_NAND_CLK_CTRL		0x168
#define CLKCTRL_SD_EMMC_CLK_CTRL	0x16c
#define CLKCTRL_SPICC_CLK_CTRL		0x174
#define CLKCTRL_SAR_CLK_CTRL0		0x17c
#define CLKCTRL_PWM_CLK_AB_CTRL		0x180
#define CLKCTRL_PWM_CLK_CD_CTRL		0x184
#define CLKCTRL_PWM_CLK_EF_CTRL		0x188
#define CLKCTRL_PWM_CLK_AO_AB_CTRL	0x1a0
#define CLKCTRL_PWM_CLK_AO_CD_CTRL	0x1a4
#define CLKCTRL_PWM_CLK_AO_EF_CTRL	0x1a8
#define CLKCTRL_PWM_CLK_AO_GH_CTRL	0x1ac
#define CLKCTRL_SPICC_CLK_CTRL1		0x1c0
#define CLKCTRL_SPICC_CLK_CTRL2		0x1c4

static struct clk_regmap rtc_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "rtc_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static const struct meson_clk_dualdiv_param clk_32k_div_table[] = {
	{
		.n1	= 733, .m1	= 8,
		.n2	= 732, .m2	= 11,
		.dual	= 1,
	},
	{}
};

static struct clk_regmap rtc_32k_div = {
	.data = &(struct meson_clk_dualdiv_data){
		.n1 = {
			.reg_off = CLKCTRL_RTC_BY_OSCIN_CTRL0,
			.shift   = 0,
			.width   = 12,
		},
		.n2 = {
			.reg_off = CLKCTRL_RTC_BY_OSCIN_CTRL0,
			.shift   = 12,
			.width   = 12,
		},
		.m1 = {
			.reg_off = CLKCTRL_RTC_BY_OSCIN_CTRL1,
			.shift   = 0,
			.width   = 12,
		},
		.m2 = {
			.reg_off = CLKCTRL_RTC_BY_OSCIN_CTRL1,
			.shift   = 12,
			.width   = 12,
		},
		.dual = {
			.reg_off = CLKCTRL_RTC_BY_OSCIN_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.table = clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "rtc_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&rtc_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap rtc_32k_force_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "rtc_32k_force_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&rtc_32k_div.hw,
			&rtc_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap rtc_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "rtc_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&rtc_32k_force_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap rtc_32k_mux0_0 = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "rtc_32k_mux0_0",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "xtal", },
			{ .hw = &rtc_32k_out.hw },
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap rtc_32k_mux0_1 = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "rtc_32k_mux0_1",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "pad", },
			{ .fw_name = "xtal", },
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap rtc = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 1,
	},
	.hw.init = &(struct clk_init_data){
		.name = "rtc",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&rtc_32k_mux0_0.hw,
			&rtc_32k_mux0_1.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap ceca_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECA_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "ceca_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap ceca_32k_div = {
	.data = &(struct meson_clk_dualdiv_data){
		.n1 = {
			.reg_off = CLKCTRL_CECA_CTRL0,
			.shift   = 0,
			.width   = 12,
		},
		.n2 = {
			.reg_off = CLKCTRL_CECA_CTRL0,
			.shift   = 12,
			.width   = 12,
		},
		.m1 = {
			.reg_off = CLKCTRL_CECA_CTRL1,
			.shift   = 0,
			.width   = 12,
		},
		.m2 = {
			.reg_off = CLKCTRL_CECA_CTRL1,
			.shift   = 12,
			.width   = 12,
		},
		.dual = {
			.reg_off = CLKCTRL_CECA_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.table = clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ceca_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&ceca_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap ceca_32k_sel_pre = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECA_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ceca_32k_sel_pre",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&ceca_32k_div.hw,
			&ceca_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap ceca_32k_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECA_CTRL1,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ceca_32k_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&ceca_32k_sel_pre.hw,
			&rtc.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap ceca_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECA_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ceca_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&ceca_32k_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap cecb_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECB_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "cecb_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap cecb_32k_div = {
	.data = &(struct meson_clk_dualdiv_data){
		.n1 = {
			.reg_off = CLKCTRL_CECB_CTRL0,
			.shift   = 0,
			.width   = 12,
		},
		.n2 = {
			.reg_off = CLKCTRL_CECB_CTRL0,
			.shift   = 12,
			.width   = 12,
		},
		.m1 = {
			.reg_off = CLKCTRL_CECB_CTRL1,
			.shift   = 0,
			.width   = 12,
		},
		.m2 = {
			.reg_off = CLKCTRL_CECB_CTRL1,
			.shift   = 12,
			.width   = 12,
		},
		.dual = {
			.reg_off = CLKCTRL_CECB_CTRL0,
			.shift   = 28,
			.width   = 1,
		},
		.table = clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "cecb_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&cecb_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap cecb_32k_sel_pre = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECB_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "cecb_32k_sel_pre",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&cecb_32k_div.hw,
			&cecb_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap cecb_32k_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECB_CTRL1,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "cecb_32k_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&cecb_32k_sel_pre.hw,
			&rtc.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap cecb_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECB_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "cecb_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&cecb_32k_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/* Smartcard Clock */
static const struct clk_parent_data sc_parents[] = {
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "xtal", },
};

static struct clk_regmap sc_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.mask = 0x3,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sc_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = sc_parents,
		.num_parents = ARRAY_SIZE(sc_parents),
	},
};

static struct clk_regmap sc_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sc_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sc_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sc = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sc",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sc_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The DSPA/B IP is clocked by two identical clocks (dspa/b_a and dspa/b_b)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data dsp_ab_parent_data[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "hifi", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv7", },
	{ .hw = &rtc.hw },
};

static struct clk_regmap dspa_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x7,
		.shift = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspa_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(dsp_ab_parent_data),
	},
};

static struct clk_regmap dspa_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.shift = 0,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspa_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspa_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspa_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.bit_idx = 13,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "dspa_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspa_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspa_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x7,
		.shift = 26,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspa_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(dsp_ab_parent_data),
	},
};

static struct clk_regmap dspa_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.shift = 16,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspa_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspa_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspa_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.bit_idx = 29,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "dspa_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspa_b_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspa = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x1,
		.shift = 15,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspa",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspa_a.hw,
			&dspa_b.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspb_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x7,
		.shift = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspb_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(dsp_ab_parent_data),
	},
};

static struct clk_regmap dspb_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.shift = 0,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspb_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspb_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspb_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.bit_idx = 13,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "dspb_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspb_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspb_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x7,
		.shift = 26,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspb_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(dsp_ab_parent_data),
	},
};

static struct clk_regmap dspb_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.shift = 16,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspb_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspb_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspb_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.bit_idx = 29,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "dspb_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspb_b_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap dspb = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x1,
		.shift = 15,
	},
	.hw.init = &(struct clk_init_data){
		.name = "dspb",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&dspb_a.hw,
			&dspb_b.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap clk_24m = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 11,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "24m",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_fixed_factor clk_24m_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "24m_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&clk_24m.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap clk_12m = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 10,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "12m",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&clk_24m_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap fdiv2_divn_pre = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fdiv2_divn_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "fdiv2",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap fdiv2_divn = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 12,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fdiv2_divn",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&fdiv2_divn_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The NNA IP is clocked by two identical clocks (anakin_0 and anakin_1)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data anakin_parent_data[] = {
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "vid_pll0", },
	{ .fw_name = "mpll1", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "fdiv2p5", },
};

static struct clk_regmap anakin_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "anakin_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = anakin_parent_data,
		.num_parents = ARRAY_SIZE(anakin_parent_data),
	},
};

static struct clk_regmap anakin_0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "anakin_0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&anakin_0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap anakin_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "anakin_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &anakin_0_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap anakin_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "anakin_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = anakin_parent_data,
		.num_parents = ARRAY_SIZE(anakin_parent_data),
	},
};

static struct clk_regmap anakin_1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "anakin_1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&anakin_1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap anakin_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "anakin_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&anakin_1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap anakin = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "anakin_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&anakin_0.hw,
			&anakin_1.hw
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT
	},
};

static struct clk_regmap anakin_clk = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "anakin_clk",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&anakin.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT
	},
};

static const struct clk_parent_data mipi_csi_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "gp1", },
	{ .fw_name = "mpll1", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
};

static struct clk_regmap mipi_csi_phy0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_csi_phy0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = mipi_csi_parents,
		.num_parents = ARRAY_SIZE(mipi_csi_parents),
	},
};

static struct clk_regmap mipi_csi_phy0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_csi_phy0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_csi_phy0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mipi_csi_phy0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mipi_csi_phy0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_csi_phy0_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mipi_csi_phy1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_csi_phy1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = mipi_csi_parents,
		.num_parents = ARRAY_SIZE(mipi_csi_parents),
	},
};

static struct clk_regmap mipi_csi_phy1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_csi_phy1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_csi_phy1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mipi_csi_phy1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mipi_csi_phy1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_csi_phy1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mipi_csi_phy = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_csi_phy",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_csi_phy0.hw,
			&mipi_csi_phy1.hw
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data mipi_isp_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "mpll3", },
	{ .fw_name = "gp1", },
};

static struct clk_regmap mipi_isp_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_isp_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = mipi_isp_parents,
		.num_parents = ARRAY_SIZE(mipi_isp_parents),
	},
};

static struct clk_regmap mipi_isp_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mipi_isp_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_isp_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mipi_isp = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "mipi_isp",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mipi_isp_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap ts_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_TS_CLK_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ts_div",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap ts = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_TS_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "ts",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&ts_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The MALI IP is clocked by two identical clocks (mali_0 and mali_1)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data mali_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "gp0", },
	{ .fw_name = "gp1", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
};

static struct clk_regmap mali_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = mali_parents,
		.num_parents = ARRAY_SIZE(mali_parents),
	},
};

static struct clk_regmap mali_0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mali_0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mali_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mali_0_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mali_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = mali_parents,
		.num_parents = ARRAY_SIZE(mali_parents),
	},
};

static struct clk_regmap mali_1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mali_1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mali_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mali_1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap mali = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mali",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&mali_0.hw,
			&mali_1.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static u32 eth_rmii_table[] = { 0, 7 };

static const struct clk_parent_data eth_rmii_parents[] = {
	{ .fw_name = "fdiv2", },
	{ .fw_name = "rmii_pad", },
};

static struct clk_regmap eth_rmii_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.mask = 0x3,
		.shift = 9,
		.table = eth_rmii_table
	},
	.hw.init = &(struct clk_init_data){
		.name = "eth_rmii_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = eth_rmii_parents,
		.num_parents = ARRAY_SIZE(eth_rmii_parents),
		.num_parents = 2
	},
};

static struct clk_regmap eth_rmii_div = {
	.data = &(struct clk_regmap_div_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "eth_rmii_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&eth_rmii_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap eth_rmii = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "eth_rmii",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&eth_rmii_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor fdiv2_div8 = {
	.mult = 1,
	.div = 8,
	.hw.init = &(struct clk_init_data){
		.name = "fdiv2_div8",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "fdiv2",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap eth_125m = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "eth_125m",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&fdiv2_div8.hw
		},
		.num_parents = 1,
	},
};

static const struct clk_parent_data sd_emmc_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "hifi", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "mpll3", },
	{ .fw_name = "gp0", },
};

static struct clk_regmap sd_emmc_c_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = sd_emmc_parents,
		.num_parents = ARRAY_SIZE(sd_emmc_parents),
	},
};

static struct clk_regmap sd_emmc_c_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_c_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_c = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sd_emmc_c",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_c_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = sd_emmc_parents,
		.num_parents = ARRAY_SIZE(sd_emmc_parents),
	},
};

static struct clk_regmap sd_emmc_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sd_emmc_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = sd_emmc_parents,
		.num_parents = ARRAY_SIZE(sd_emmc_parents),
	},
};

static struct clk_regmap sd_emmc_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap sd_emmc_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.bit_idx = 23,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sd_emmc_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&sd_emmc_b_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

#define SPI_PWM_CLK_MUX(_name, _reg, _mask, _shift, _parent_data) {	\
	.data = &(struct clk_regmap_mux_data) {			\
		.offset = _reg,					\
		.mask = _mask,					\
		.shift = _shift,				\
	},							\
	.hw.init = &(struct clk_init_data) {			\
		.name = #_name "_sel",				\
		.ops = &clk_regmap_mux_ops,			\
		.parent_data = _parent_data,			\
		.num_parents = ARRAY_SIZE(_parent_data),	\
	},							\
}

#define SPI_PWM_CLK_DIV(_name, _reg, _shift, _width, _parent) {	\
	.data = &(struct clk_regmap_div_data) {			\
		.offset = _reg,					\
		.shift = _shift,				\
		.width = _width,				\
	},							\
	.hw.init = &(struct clk_init_data) {			\
		.name = #_name "_div",				\
		.ops = &clk_regmap_divider_ops,			\
		.parent_hws = (const struct clk_hw *[]) {	\
			&_parent.hw				\
		},						\
		.num_parents = 1,				\
		.flags = CLK_SET_RATE_PARENT,			\
	},							\
}

#define SPI_PWM_CLK_GATE(_name, _reg, _bit, _parent) {		\
	.data = &(struct clk_regmap_gate_data) {		\
		.offset = _reg,					\
		.bit_idx = _bit,				\
	},							\
	.hw.init = &(struct clk_init_data) {			\
		.name = #_name,					\
		.ops = &clk_regmap_gate_ops,			\
		.parent_hws = (const struct clk_hw *[]) {	\
			&_parent.hw				\
		},						\
		.num_parents = 1,				\
		.flags = CLK_SET_RATE_PARENT,			\
	},							\
}

static const struct clk_parent_data spicc_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "sys", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
	{ .fw_name = "gp1", },
};

static struct clk_regmap spicc0_sel =
	SPI_PWM_CLK_MUX(spicc0, CLKCTRL_SPICC_CLK_CTRL, 0x7, 7, spicc_parents);
static struct clk_regmap spicc0_div = SPI_PWM_CLK_DIV(spicc0, CLKCTRL_SPICC_CLK_CTRL, 0, 6, spicc0_sel);
static struct clk_regmap spicc0 = SPI_PWM_CLK_GATE(spicc0, CLKCTRL_SPICC_CLK_CTRL, 6, spicc0_div);

static struct clk_regmap spicc1_sel =
	SPI_PWM_CLK_MUX(spicc1, CLKCTRL_SPICC_CLK_CTRL, 0x7, 23, spicc_parents);
static struct clk_regmap spicc1_div = SPI_PWM_CLK_DIV(spicc1, CLKCTRL_SPICC_CLK_CTRL, 16, 6, spicc1_sel);
static struct clk_regmap spicc1 = SPI_PWM_CLK_GATE(spicc1, CLKCTRL_SPICC_CLK_CTRL, 22, spicc1_div);

static struct clk_regmap spicc2_sel =
	SPI_PWM_CLK_MUX(spicc2, CLKCTRL_SPICC_CLK_CTRL1, 0x7, 7, spicc_parents);
static struct clk_regmap spicc2_div = SPI_PWM_CLK_DIV(spicc2, CLKCTRL_SPICC_CLK_CTRL1, 0, 6, spicc2_sel);
static struct clk_regmap spicc2 = SPI_PWM_CLK_GATE(spicc2, CLKCTRL_SPICC_CLK_CTRL1, 6, spicc2_div);

static struct clk_regmap spicc3_sel =
	SPI_PWM_CLK_MUX(spicc3, CLKCTRL_SPICC_CLK_CTRL1, 0x7, 23, spicc_parents);
static struct clk_regmap spicc3_div = SPI_PWM_CLK_DIV(spicc3, CLKCTRL_SPICC_CLK_CTRL1, 16, 6, spicc3_sel);
static struct clk_regmap spicc3 = SPI_PWM_CLK_GATE(spicc3, CLKCTRL_SPICC_CLK_CTRL1, 22, spicc3_div);

static struct clk_regmap spicc4_sel =
	SPI_PWM_CLK_MUX(spicc4, CLKCTRL_SPICC_CLK_CTRL2, 0x7, 7, spicc_parents);
static struct clk_regmap spicc4_div = SPI_PWM_CLK_DIV(spicc4, CLKCTRL_SPICC_CLK_CTRL2, 0, 6, spicc4_sel);
static struct clk_regmap spicc4 = SPI_PWM_CLK_GATE(spicc4, CLKCTRL_SPICC_CLK_CTRL2, 6, spicc4_div);

static struct clk_regmap spicc5_sel =
	SPI_PWM_CLK_MUX(spicc5, CLKCTRL_SPICC_CLK_CTRL2, 0x7, 23, spicc_parents);
static struct clk_regmap spicc5_div = SPI_PWM_CLK_DIV(spicc5, CLKCTRL_SPICC_CLK_CTRL2, 16, 6, spicc5_sel);
static struct clk_regmap spicc5 = SPI_PWM_CLK_GATE(spicc5, CLKCTRL_SPICC_CLK_CTRL2, 22, spicc5_div);

static struct clk_regmap saradc_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.mask = 0x1,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "saradc_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "xtal", },
			{ .fw_name = "sys", },
		},
		.num_parents = 2,
	},
};

static struct clk_regmap saradc_div = {
	.data = &(struct clk_regmap_div_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "saradc_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&saradc_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap saradc = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "saradc",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&saradc_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data pwm_parents[]  = {
	{ .fw_name = "xtal", },
	{ .fw_name = "vid_pll0", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
};

static struct clk_regmap pwm_a_sel =
	SPI_PWM_CLK_MUX(pwm_a, CLKCTRL_PWM_CLK_AB_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_a_div = SPI_PWM_CLK_DIV(pwm_a, CLKCTRL_PWM_CLK_AB_CTRL, 0, 8, pwm_a_sel);
static struct clk_regmap pwm_a = SPI_PWM_CLK_GATE(pwm_a, CLKCTRL_PWM_CLK_AB_CTRL, 8, pwm_a_div);

static struct clk_regmap pwm_b_sel =
	SPI_PWM_CLK_MUX(pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_b_div = SPI_PWM_CLK_DIV(pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 16, 8, pwm_b_sel);
static struct clk_regmap pwm_b = SPI_PWM_CLK_GATE(pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 24, pwm_b_div);

static struct clk_regmap pwm_c_sel =
	SPI_PWM_CLK_MUX(pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_c_div = SPI_PWM_CLK_DIV(pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 0, 8, pwm_c_sel);
static struct clk_regmap pwm_c = SPI_PWM_CLK_GATE(pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 8, pwm_c_div);

static struct clk_regmap pwm_d_sel =
	SPI_PWM_CLK_MUX(pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_d_div = SPI_PWM_CLK_DIV(pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 16, 8, pwm_d_sel);
static struct clk_regmap pwm_d = SPI_PWM_CLK_GATE(pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 24, pwm_d_div);

static struct clk_regmap pwm_e_sel =
	SPI_PWM_CLK_MUX(pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_e_div = SPI_PWM_CLK_DIV(pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 0, 8, pwm_e_sel);
static struct clk_regmap pwm_e = SPI_PWM_CLK_GATE(pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 8, pwm_e_div);

static struct clk_regmap pwm_f_sel =
	SPI_PWM_CLK_MUX(pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_f_div = SPI_PWM_CLK_DIV(pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 16, 8, pwm_f_sel);
static struct clk_regmap pwm_f = SPI_PWM_CLK_GATE(pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 24, pwm_f_div);

static struct clk_regmap pwm_ao_a_sel =
	SPI_PWM_CLK_MUX(pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_ao_a_div = SPI_PWM_CLK_DIV(pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0, 8, pwm_ao_a_sel);
static struct clk_regmap pwm_ao_a = SPI_PWM_CLK_GATE(pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 8, pwm_ao_a_div);

static struct clk_regmap pwm_ao_b_sel =
	SPI_PWM_CLK_MUX(pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_ao_b_div = SPI_PWM_CLK_DIV(pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 16, 8, pwm_ao_b_sel);
static struct clk_regmap pwm_ao_b = SPI_PWM_CLK_GATE(pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 24, pwm_ao_b_div);

static struct clk_regmap pwm_ao_c_sel =
	SPI_PWM_CLK_MUX(pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_ao_c_div = SPI_PWM_CLK_DIV(pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0, 8, pwm_ao_c_sel);
static struct clk_regmap pwm_ao_c = SPI_PWM_CLK_GATE(pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 8, pwm_ao_c_div);

static struct clk_regmap pwm_ao_d_sel =
	SPI_PWM_CLK_MUX(pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_ao_d_div = SPI_PWM_CLK_DIV(pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 16, 8, pwm_ao_d_sel);
static struct clk_regmap pwm_ao_d = SPI_PWM_CLK_GATE(pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 24, pwm_ao_d_div);

static struct clk_regmap pwm_ao_e_sel =
	SPI_PWM_CLK_MUX(pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_ao_e_div = SPI_PWM_CLK_DIV(pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0, 8, pwm_ao_e_sel);
static struct clk_regmap pwm_ao_e = SPI_PWM_CLK_GATE(pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 8, pwm_ao_e_div);

static struct clk_regmap pwm_ao_f_sel =
	SPI_PWM_CLK_MUX(pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_ao_f_div = SPI_PWM_CLK_DIV(pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 16, 8, pwm_ao_f_sel);
static struct clk_regmap pwm_ao_f = SPI_PWM_CLK_GATE(pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 24, pwm_ao_f_div);

static struct clk_regmap pwm_ao_g_sel =
	SPI_PWM_CLK_MUX(pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0x3, 9, pwm_parents);
static struct clk_regmap pwm_ao_g_div = SPI_PWM_CLK_DIV(pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0, 8, pwm_ao_g_sel);
static struct clk_regmap pwm_ao_g = SPI_PWM_CLK_GATE(pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 8, pwm_ao_g_div);

static struct clk_regmap pwm_ao_h_sel =
	SPI_PWM_CLK_MUX(pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0x3, 25, pwm_parents);
static struct clk_regmap pwm_ao_h_div = SPI_PWM_CLK_DIV(pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 16, 8, pwm_ao_h_sel);
static struct clk_regmap pwm_ao_h = SPI_PWM_CLK_GATE(pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 24, pwm_ao_h_div);

#define T7_CLK_GATE(_name, _reg, _bit, _fw_name, _flags)		\
struct clk_regmap _name = {						\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = (_reg),					\
		.bit_idx = (_bit),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &clk_regmap_gate_ops,				\
		.parent_data = &(const struct clk_parent_data) {	\
			.fw_name = #_fw_name,				\
		},							\
		.num_parents = 1,					\
		.flags = (_flags),					\
	},								\
}

#define T7_SYS_GATE(_name, _reg, _bit, _flags)				\
	T7_CLK_GATE(_name, _reg, _bit, sys, _flags)

static T7_SYS_GATE(sys_ddr,		CLKCTRL_SYS_CLK_EN0_REG0, 0,	0);
static T7_SYS_GATE(sys_dos,		CLKCTRL_SYS_CLK_EN0_REG0, 1,	0);
static T7_SYS_GATE(sys_mipi_dsi_a,	CLKCTRL_SYS_CLK_EN0_REG0, 2,	0);
static T7_SYS_GATE(sys_mipi_dsi_b,	CLKCTRL_SYS_CLK_EN0_REG0, 3,	0);
static T7_SYS_GATE(sys_ethphy,		CLKCTRL_SYS_CLK_EN0_REG0, 4,	0);
static T7_SYS_GATE(sys_mali,		CLKCTRL_SYS_CLK_EN0_REG0, 6,	0);
static T7_SYS_GATE(sys_aocpu,		CLKCTRL_SYS_CLK_EN0_REG0, 13,	0);
static T7_SYS_GATE(sys_aucpu,		CLKCTRL_SYS_CLK_EN0_REG0, 14,	0);
static T7_SYS_GATE(sys_cec,		CLKCTRL_SYS_CLK_EN0_REG0, 16,	0);
static T7_SYS_GATE(sys_gdc,		CLKCTRL_SYS_CLK_EN0_REG0, 17,	0);
static T7_SYS_GATE(sys_deswarp,		CLKCTRL_SYS_CLK_EN0_REG0, 18,	0);
static T7_SYS_GATE(sys_ampipe_nand,	CLKCTRL_SYS_CLK_EN0_REG0, 19,	0);
static T7_SYS_GATE(sys_ampipe_eth,	CLKCTRL_SYS_CLK_EN0_REG0, 20,	0);
static T7_SYS_GATE(sys_am2axi0,		CLKCTRL_SYS_CLK_EN0_REG0, 21,	0);
static T7_SYS_GATE(sys_am2axi1,		CLKCTRL_SYS_CLK_EN0_REG0, 22,	0);
static T7_SYS_GATE(sys_am2axi2,		CLKCTRL_SYS_CLK_EN0_REG0, 23,	0);
static T7_SYS_GATE(sys_sdemmca,		CLKCTRL_SYS_CLK_EN0_REG0, 24,	0);
static T7_SYS_GATE(sys_sdemmcb,		CLKCTRL_SYS_CLK_EN0_REG0, 25,	0);
static T7_SYS_GATE(sys_sdemmcc,		CLKCTRL_SYS_CLK_EN0_REG0, 26,	0);
static T7_SYS_GATE(sys_smartcard,	CLKCTRL_SYS_CLK_EN0_REG0, 27,	0);
static T7_SYS_GATE(sys_acodec,		CLKCTRL_SYS_CLK_EN0_REG0, 28,	0);
static T7_SYS_GATE(sys_spifc,		CLKCTRL_SYS_CLK_EN0_REG0, 29,	0);
static T7_SYS_GATE(sys_msr_clk,		CLKCTRL_SYS_CLK_EN0_REG0, 30,	0);
static T7_SYS_GATE(sys_ir_ctrl,		CLKCTRL_SYS_CLK_EN0_REG0, 31,	0);
static T7_SYS_GATE(sys_audio,		CLKCTRL_SYS_CLK_EN0_REG1, 0,	0);
static T7_SYS_GATE(sys_eth,		CLKCTRL_SYS_CLK_EN0_REG1, 3,	0);
static T7_SYS_GATE(sys_uart_a,		CLKCTRL_SYS_CLK_EN0_REG1, 5,	0);
static T7_SYS_GATE(sys_uart_b,		CLKCTRL_SYS_CLK_EN0_REG1, 6,	0);
static T7_SYS_GATE(sys_uart_c,		CLKCTRL_SYS_CLK_EN0_REG1, 7,	0);
static T7_SYS_GATE(sys_uart_d,		CLKCTRL_SYS_CLK_EN0_REG1, 8,	0);
static T7_SYS_GATE(sys_uart_e,		CLKCTRL_SYS_CLK_EN0_REG1, 9,	0);
static T7_SYS_GATE(sys_uart_f,		CLKCTRL_SYS_CLK_EN0_REG1, 10,	0);
static T7_SYS_GATE(sys_aififo,		CLKCTRL_SYS_CLK_EN0_REG1, 11,	0);
static T7_SYS_GATE(sys_spicc2,		CLKCTRL_SYS_CLK_EN0_REG1, 12,	0);
static T7_SYS_GATE(sys_spicc3,		CLKCTRL_SYS_CLK_EN0_REG1, 13,	0);
static T7_SYS_GATE(sys_spicc4,		CLKCTRL_SYS_CLK_EN0_REG1, 14,	0);
static T7_SYS_GATE(sys_ts_a73,		CLKCTRL_SYS_CLK_EN0_REG1, 15,	0);
static T7_SYS_GATE(sys_ts_a53,		CLKCTRL_SYS_CLK_EN0_REG1, 16,	0);
static T7_SYS_GATE(sys_spicc5,		CLKCTRL_SYS_CLK_EN0_REG1, 17,	0);
static T7_SYS_GATE(sys_g2d,		CLKCTRL_SYS_CLK_EN0_REG1, 20,	0);
static T7_SYS_GATE(sys_spicc0,		CLKCTRL_SYS_CLK_EN0_REG1, 21,	0);
static T7_SYS_GATE(sys_spicc1,		CLKCTRL_SYS_CLK_EN0_REG1, 22,	0);
static T7_SYS_GATE(sys_pcie,		CLKCTRL_SYS_CLK_EN0_REG1, 24,	0);
static T7_SYS_GATE(sys_usb,		CLKCTRL_SYS_CLK_EN0_REG1, 26,	0);
static T7_SYS_GATE(sys_pcie_phy,	CLKCTRL_SYS_CLK_EN0_REG1, 27,	0);
static T7_SYS_GATE(sys_i2c_ao_a,	CLKCTRL_SYS_CLK_EN0_REG1, 28,	0);
static T7_SYS_GATE(sys_i2c_ao_b,	CLKCTRL_SYS_CLK_EN0_REG1, 29,	0);
static T7_SYS_GATE(sys_i2c_m_a,		CLKCTRL_SYS_CLK_EN0_REG1, 30,	0);
static T7_SYS_GATE(sys_i2c_m_b,		CLKCTRL_SYS_CLK_EN0_REG1, 31,	0);
static T7_SYS_GATE(sys_i2c_m_c,		CLKCTRL_SYS_CLK_EN0_REG2, 0,	0);
static T7_SYS_GATE(sys_i2c_m_d,		CLKCTRL_SYS_CLK_EN0_REG2, 1,	0);
static T7_SYS_GATE(sys_i2c_m_e,		CLKCTRL_SYS_CLK_EN0_REG2, 2,	0);
static T7_SYS_GATE(sys_i2c_m_f,		CLKCTRL_SYS_CLK_EN0_REG2, 3,	0);
static T7_SYS_GATE(sys_hdmitx_apb,	CLKCTRL_SYS_CLK_EN0_REG2, 4,	0);
static T7_SYS_GATE(sys_i2c_s_a,		CLKCTRL_SYS_CLK_EN0_REG2, 5,	0);
static T7_SYS_GATE(sys_hdmirx_pclk,	CLKCTRL_SYS_CLK_EN0_REG2, 8,	0);
static T7_SYS_GATE(sys_mmc_apb,		CLKCTRL_SYS_CLK_EN0_REG2, 11,	0);
static T7_SYS_GATE(sys_mipi_isp_pclk,	CLKCTRL_SYS_CLK_EN0_REG2, 17,	0);
static T7_SYS_GATE(sys_rsa,		CLKCTRL_SYS_CLK_EN0_REG2, 18,	0);
static T7_SYS_GATE(sys_pclk_sys_apb,	CLKCTRL_SYS_CLK_EN0_REG2, 19,	0);
static T7_SYS_GATE(sys_a73pclk_apb,	CLKCTRL_SYS_CLK_EN0_REG2, 20,	0);
static T7_SYS_GATE(sys_dspa,		CLKCTRL_SYS_CLK_EN0_REG2, 21,	0);
static T7_SYS_GATE(sys_dspb,		CLKCTRL_SYS_CLK_EN0_REG2, 22,	0);
static T7_SYS_GATE(sys_vpu_intr,	CLKCTRL_SYS_CLK_EN0_REG2, 25,	0);
static T7_SYS_GATE(sys_sar_adc,		CLKCTRL_SYS_CLK_EN0_REG2, 28,	0);
/*
 * sys_gic provides the clock for GIC(Generic Interrupt Controller).
 * After clock is disabled, The GIC cannot work properly. At present, the driver
 * used by our GIC is the public driver in kernel, and there is no management
 * clock in the driver.
 */
static T7_SYS_GATE(sys_gic,		CLKCTRL_SYS_CLK_EN0_REG2, 30,	CLK_IS_CRITICAL);
static T7_SYS_GATE(sys_ts_gpu,		CLKCTRL_SYS_CLK_EN0_REG2, 31,	0);
static T7_SYS_GATE(sys_ts_nna,		CLKCTRL_SYS_CLK_EN0_REG3, 0,	0);
static T7_SYS_GATE(sys_ts_vpu,		CLKCTRL_SYS_CLK_EN0_REG3, 1,	0);
static T7_SYS_GATE(sys_ts_hevc,		CLKCTRL_SYS_CLK_EN0_REG3, 2,	0);
static T7_SYS_GATE(sys_pwm_ao_ab,	CLKCTRL_SYS_CLK_EN0_REG3, 3,	0);
static T7_SYS_GATE(sys_pwm_ao_cd,	CLKCTRL_SYS_CLK_EN0_REG3, 4,	0);
static T7_SYS_GATE(sys_pwm_ao_ef,	CLKCTRL_SYS_CLK_EN0_REG3, 5,	0);
static T7_SYS_GATE(sys_pwm_ao_gh,	CLKCTRL_SYS_CLK_EN0_REG3, 6,	0);
static T7_SYS_GATE(sys_pwm_ab,		CLKCTRL_SYS_CLK_EN0_REG3, 7,	0);
static T7_SYS_GATE(sys_pwm_cd,		CLKCTRL_SYS_CLK_EN0_REG3, 8,	0);
static T7_SYS_GATE(sys_pwm_ef,		CLKCTRL_SYS_CLK_EN0_REG3, 9,	0);

/* Array of all clocks registered by this provider */
static struct clk_hw *t7_periphs_hw_clks[] = {
	[CLKID_RTC_32K_IN]		= &rtc_32k_in.hw,
	[CLKID_RTC_32K_DIV]		= &rtc_32k_div.hw,
	[CLKID_RTC_32K_FORCE_SEL]	= &rtc_32k_force_sel.hw,
	[CLKID_RTC_32K_OUT]		= &rtc_32k_out.hw,
	[CLKID_RTC_32K_MUX0_0]		= &rtc_32k_mux0_0.hw,
	[CLKID_RTC_32K_MUX0_1]		= &rtc_32k_mux0_1.hw,
	[CLKID_RTC]			= &rtc.hw,
	[CLKID_CECB_32K_IN]		= &cecb_32k_in.hw,
	[CLKID_CECB_32K_DIV]		= &cecb_32k_div.hw,
	[CLKID_CECB_32K_SEL_PRE]	= &cecb_32k_sel_pre.hw,
	[CLKID_CECB_32K_SEL]		= &cecb_32k_sel.hw,
	[CLKID_CECA_32K_IN]		= &ceca_32k_in.hw,
	[CLKID_CECA_32K_DIV]		= &ceca_32k_div.hw,
	[CLKID_CECA_32K_SEL_PRE]	= &ceca_32k_sel_pre.hw,
	[CLKID_CECA_32K_SEL]		= &ceca_32k_sel.hw,
	[CLKID_CECA_32K]		= &ceca_32k_out.hw,
	[CLKID_CECB_32K]		= &cecb_32k_out.hw,
	[CLKID_SC_SEL]			= &sc_sel.hw,
	[CLKID_SC_DIV]			= &sc_div.hw,
	[CLKID_SC]			= &sc.hw,
	[CLKID_DSPA_A_SEL]		= &dspa_a_sel.hw,
	[CLKID_DSPA_A_DIV]		= &dspa_a_div.hw,
	[CLKID_DSPA_A]			= &dspa_a.hw,
	[CLKID_DSPA_B_SEL]		= &dspa_b_sel.hw,
	[CLKID_DSPA_B_DIV]		= &dspa_b_div.hw,
	[CLKID_DSPA_B]			= &dspa_b.hw,
	[CLKID_DSPA]			= &dspa.hw,
	[CLKID_DSPB_A_SEL]		= &dspb_a_sel.hw,
	[CLKID_DSPB_A_DIV]		= &dspb_a_div.hw,
	[CLKID_DSPB_A]			= &dspb_a.hw,
	[CLKID_DSPB_B_SEL]		= &dspb_b_sel.hw,
	[CLKID_DSPB_B_DIV]		= &dspb_b_div.hw,
	[CLKID_DSPB_B]			= &dspb_b.hw,
	[CLKID_DSPB]			= &dspb.hw,
	[CLKID_CLK_24M]			= &clk_24m.hw,
	[CLKID_CLK_24M_DIV2]		= &clk_24m_div2.hw,
	[CLKID_CLK_12M]			= &clk_12m.hw,
	[CLKID_ANAKIN_0_SEL]		= &anakin_0_sel.hw,
	[CLKID_ANAKIN_0_DIV]		= &anakin_0_div.hw,
	[CLKID_ANAKIN_0]		= &anakin_0.hw,
	[CLKID_ANAKIN_1_SEL]		= &anakin_1_sel.hw,
	[CLKID_ANAKIN_1_DIV]		= &anakin_1_div.hw,
	[CLKID_ANAKIN_1]		= &anakin_1.hw,
	[CLKID_ANAKIN]			= &anakin.hw,
	[CLKID_ANAKIN_CLK]		= &anakin_clk.hw,
	[CLKID_FCLK_DIV2_DIVN_PRE]	= &fdiv2_divn_pre.hw,
	[CLKID_FCLK_DIV2_DIVN]		= &fdiv2_divn.hw,
	[CLKID_MIPI_CSI_PHY_0_SEL]	= &mipi_csi_phy0_sel.hw,
	[CLKID_MIPI_CSI_PHY_0_DIV]	= &mipi_csi_phy0_div.hw,
	[CLKID_MIPI_CSI_PHY_0]		= &mipi_csi_phy0.hw,
	[CLKID_MIPI_CSI_PHY_1_SEL]	= &mipi_csi_phy1_sel.hw,
	[CLKID_MIPI_CSI_PHY_1_DIV]	= &mipi_csi_phy1_div.hw,
	[CLKID_MIPI_CSI_PHY_1]		= &mipi_csi_phy1.hw,
	[CLKID_MIPI_CSI_PHY]		= &mipi_csi_phy.hw,
	[CLKID_MIPI_ISP_SEL]		= &mipi_isp_sel.hw,
	[CLKID_MIPI_ISP_DIV]		= &mipi_isp_div.hw,
	[CLKID_MIPI_ISP]		= &mipi_isp.hw,
	[CLKID_TS_DIV]			= &ts_div.hw,
	[CLKID_TS]			= &ts.hw,
	[CLKID_MALI_0_SEL]		= &mali_0_sel.hw,
	[CLKID_MALI_0_DIV]		= &mali_0_div.hw,
	[CLKID_MALI_0]			= &mali_0.hw,
	[CLKID_MALI_1_SEL]		= &mali_1_sel.hw,
	[CLKID_MALI_1_DIV]		= &mali_1_div.hw,
	[CLKID_MALI_1]			= &mali_1.hw,
	[CLKID_MALI]			= &mali.hw,
	[CLKID_ETH_RMII_SEL]		= &eth_rmii_sel.hw,
	[CLKID_ETH_RMII_DIV]		= &eth_rmii_div.hw,
	[CLKID_ETH_RMII]		= &eth_rmii.hw,
	[CLKID_FCLK_DIV2_DIV8]		= &fdiv2_div8.hw,
	[CLKID_ETH_125M]		= &eth_125m.hw,
	[CLKID_SD_EMMC_C_SEL]		= &sd_emmc_c_sel.hw,
	[CLKID_SD_EMMC_C_DIV]		= &sd_emmc_c_div.hw,
	[CLKID_SD_EMMC_C]		= &sd_emmc_c.hw,
	[CLKID_SD_EMMC_A_SEL]		= &sd_emmc_a_sel.hw,
	[CLKID_SD_EMMC_A_DIV]		= &sd_emmc_a_div.hw,
	[CLKID_SD_EMMC_A]		= &sd_emmc_a.hw,
	[CLKID_SD_EMMC_B_SEL]		= &sd_emmc_b_sel.hw,
	[CLKID_SD_EMMC_B_DIV]		= &sd_emmc_b_div.hw,
	[CLKID_SD_EMMC_B]		= &sd_emmc_b.hw,
	[CLKID_SPICC0_SEL]		= &spicc0_sel.hw,
	[CLKID_SPICC0_DIV]		= &spicc0_div.hw,
	[CLKID_SPICC0]			= &spicc0.hw,
	[CLKID_SPICC1_SEL]		= &spicc1_sel.hw,
	[CLKID_SPICC1_DIV]		= &spicc1_div.hw,
	[CLKID_SPICC1]			= &spicc1.hw,
	[CLKID_SPICC2_SEL]		= &spicc2_sel.hw,
	[CLKID_SPICC2_DIV]		= &spicc2_div.hw,
	[CLKID_SPICC2]			= &spicc2.hw,
	[CLKID_SPICC3_SEL]		= &spicc3_sel.hw,
	[CLKID_SPICC3_DIV]		= &spicc3_div.hw,
	[CLKID_SPICC3]			= &spicc3.hw,
	[CLKID_SPICC4_SEL]		= &spicc4_sel.hw,
	[CLKID_SPICC4_DIV]		= &spicc4_div.hw,
	[CLKID_SPICC4]			= &spicc4.hw,
	[CLKID_SPICC5_SEL]		= &spicc5_sel.hw,
	[CLKID_SPICC5_DIV]		= &spicc5_div.hw,
	[CLKID_SPICC5]			= &spicc5.hw,
	[CLKID_SARADC_SEL]		= &saradc_sel.hw,
	[CLKID_SARADC_DIV]		= &saradc_div.hw,
	[CLKID_SARADC]			= &saradc.hw,
	[CLKID_PWM_A_SEL]		= &pwm_a_sel.hw,
	[CLKID_PWM_A_DIV]		= &pwm_a_div.hw,
	[CLKID_PWM_A]			= &pwm_a.hw,
	[CLKID_PWM_B_SEL]		= &pwm_b_sel.hw,
	[CLKID_PWM_B_DIV]		= &pwm_b_div.hw,
	[CLKID_PWM_B]			= &pwm_b.hw,
	[CLKID_PWM_C_SEL]		= &pwm_c_sel.hw,
	[CLKID_PWM_C_DIV]		= &pwm_c_div.hw,
	[CLKID_PWM_C]			= &pwm_c.hw,
	[CLKID_PWM_D_SEL]		= &pwm_d_sel.hw,
	[CLKID_PWM_D_DIV]		= &pwm_d_div.hw,
	[CLKID_PWM_D]			= &pwm_d.hw,
	[CLKID_PWM_E_SEL]		= &pwm_e_sel.hw,
	[CLKID_PWM_E_DIV]		= &pwm_e_div.hw,
	[CLKID_PWM_E]			= &pwm_e.hw,
	[CLKID_PWM_F_SEL]		= &pwm_f_sel.hw,
	[CLKID_PWM_F_DIV]		= &pwm_f_div.hw,
	[CLKID_PWM_F]			= &pwm_f.hw,
	[CLKID_PWM_AO_A_SEL]		= &pwm_ao_a_sel.hw,
	[CLKID_PWM_AO_A_DIV]		= &pwm_ao_a_div.hw,
	[CLKID_PWM_AO_A]		= &pwm_ao_a.hw,
	[CLKID_PWM_AO_B_SEL]		= &pwm_ao_b_sel.hw,
	[CLKID_PWM_AO_B_DIV]		= &pwm_ao_b_div.hw,
	[CLKID_PWM_AO_B]		= &pwm_ao_b.hw,
	[CLKID_PWM_AO_C_SEL]		= &pwm_ao_c_sel.hw,
	[CLKID_PWM_AO_C_DIV]		= &pwm_ao_c_div.hw,
	[CLKID_PWM_AO_C]		= &pwm_ao_c.hw,
	[CLKID_PWM_AO_D_SEL]		= &pwm_ao_d_sel.hw,
	[CLKID_PWM_AO_D_DIV]		= &pwm_ao_d_div.hw,
	[CLKID_PWM_AO_D]		= &pwm_ao_d.hw,
	[CLKID_PWM_AO_E_SEL]		= &pwm_ao_e_sel.hw,
	[CLKID_PWM_AO_E_DIV]		= &pwm_ao_e_div.hw,
	[CLKID_PWM_AO_E]		= &pwm_ao_e.hw,
	[CLKID_PWM_AO_F_SEL]		= &pwm_ao_f_sel.hw,
	[CLKID_PWM_AO_F_DIV]		= &pwm_ao_f_div.hw,
	[CLKID_PWM_AO_F]		= &pwm_ao_f.hw,
	[CLKID_PWM_AO_G_SEL]		= &pwm_ao_g_sel.hw,
	[CLKID_PWM_AO_G_DIV]		= &pwm_ao_g_div.hw,
	[CLKID_PWM_AO_G]		= &pwm_ao_g.hw,
	[CLKID_PWM_AO_H_SEL]		= &pwm_ao_h_sel.hw,
	[CLKID_PWM_AO_H_DIV]		= &pwm_ao_h_div.hw,
	[CLKID_PWM_AO_H]		= &pwm_ao_h.hw,
	[CLKID_SYS_DDR]			= &sys_ddr.hw,
	[CLKID_SYS_DOS]			= &sys_dos.hw,
	[CLKID_SYS_MIPI_DSI_A]		= &sys_mipi_dsi_a.hw,
	[CLKID_SYS_MIPI_DSI_B]		= &sys_mipi_dsi_b.hw,
	[CLKID_SYS_ETHPHY]		= &sys_ethphy.hw,
	[CLKID_SYS_MALI]		= &sys_mali.hw,
	[CLKID_SYS_AOCPU]		= &sys_aocpu.hw,
	[CLKID_SYS_AUCPU]		= &sys_aucpu.hw,
	[CLKID_SYS_CEC]			= &sys_cec.hw,
	[CLKID_SYS_GDC]			= &sys_gdc.hw,
	[CLKID_SYS_DESWARP]		= &sys_deswarp.hw,
	[CLKID_SYS_AMPIPE_NAND]		= &sys_ampipe_nand.hw,
	[CLKID_SYS_AMPIPE_ETH]		= &sys_ampipe_eth.hw,
	[CLKID_SYS_AM2AXI0]		= &sys_am2axi0.hw,
	[CLKID_SYS_AM2AXI1]		= &sys_am2axi1.hw,
	[CLKID_SYS_AM2AXI2]		= &sys_am2axi2.hw,
	[CLKID_SYS_SD_EMMC_A]		= &sys_sdemmca.hw,
	[CLKID_SYS_SD_EMMC_B]		= &sys_sdemmcb.hw,
	[CLKID_SYS_SD_EMMC_C]		= &sys_sdemmcc.hw,
	[CLKID_SYS_SMARTCARD]		= &sys_smartcard.hw,
	[CLKID_SYS_ACODEC]		= &sys_acodec.hw,
	[CLKID_SYS_SPIFC]		= &sys_spifc.hw,
	[CLKID_SYS_MSR_CLK]		= &sys_msr_clk.hw,
	[CLKID_SYS_IR_CTRL]		= &sys_ir_ctrl.hw,
	[CLKID_SYS_AUDIO]		= &sys_audio.hw,
	[CLKID_SYS_ETH]			= &sys_eth.hw,
	[CLKID_SYS_UART_A]		= &sys_uart_a.hw,
	[CLKID_SYS_UART_B]		= &sys_uart_b.hw,
	[CLKID_SYS_UART_C]		= &sys_uart_c.hw,
	[CLKID_SYS_UART_D]		= &sys_uart_d.hw,
	[CLKID_SYS_UART_E]		= &sys_uart_e.hw,
	[CLKID_SYS_UART_F]		= &sys_uart_f.hw,
	[CLKID_SYS_AIFIFO]		= &sys_aififo.hw,
	[CLKID_SYS_SPICC2]		= &sys_spicc2.hw,
	[CLKID_SYS_SPICC3]		= &sys_spicc3.hw,
	[CLKID_SYS_SPICC4]		= &sys_spicc4.hw,
	[CLKID_SYS_TS_A73]		= &sys_ts_a73.hw,
	[CLKID_SYS_TS_A53]		= &sys_ts_a53.hw,
	[CLKID_SYS_SPICC5]		= &sys_spicc5.hw,
	[CLKID_SYS_G2D]			= &sys_g2d.hw,
	[CLKID_SYS_SPICC0]		= &sys_spicc0.hw,
	[CLKID_SYS_SPICC1]		= &sys_spicc1.hw,
	[CLKID_SYS_PCIE]		= &sys_pcie.hw,
	[CLKID_SYS_USB]			= &sys_usb.hw,
	[CLKID_SYS_PCIE_PHY]		= &sys_pcie_phy.hw,
	[CLKID_SYS_I2C_AO_A]		= &sys_i2c_ao_a.hw,
	[CLKID_SYS_I2C_AO_B]		= &sys_i2c_ao_b.hw,
	[CLKID_SYS_I2C_M_A]		= &sys_i2c_m_a.hw,
	[CLKID_SYS_I2C_M_B]		= &sys_i2c_m_b.hw,
	[CLKID_SYS_I2C_M_C]		= &sys_i2c_m_c.hw,
	[CLKID_SYS_I2C_M_D]		= &sys_i2c_m_d.hw,
	[CLKID_SYS_I2C_M_E]		= &sys_i2c_m_e.hw,
	[CLKID_SYS_I2C_M_F]		= &sys_i2c_m_f.hw,
	[CLKID_SYS_HDMITX_APB]		= &sys_hdmitx_apb.hw,
	[CLKID_SYS_I2C_S_A]		= &sys_i2c_s_a.hw,
	[CLKID_SYS_HDMIRX_PCLK]		= &sys_hdmirx_pclk.hw,
	[CLKID_SYS_MMC_APB]		= &sys_mmc_apb.hw,
	[CLKID_SYS_MIPI_ISP_PCLK]	= &sys_mipi_isp_pclk.hw,
	[CLKID_SYS_RSA]			= &sys_rsa.hw,
	[CLKID_SYS_PCLK_SYS_APB]	= &sys_pclk_sys_apb.hw,
	[CLKID_SYS_A73PCLK_APB]		= &sys_a73pclk_apb.hw,
	[CLKID_SYS_DSPA]		= &sys_dspa.hw,
	[CLKID_SYS_DSPB]		= &sys_dspb.hw,
	[CLKID_SYS_VPU_INTR]		= &sys_vpu_intr.hw,
	[CLKID_SYS_SAR_ADC]		= &sys_sar_adc.hw,
	[CLKID_SYS_GIC]			= &sys_gic.hw,
	[CLKID_SYS_TS_GPU]		= &sys_ts_gpu.hw,
	[CLKID_SYS_TS_NNA]		= &sys_ts_nna.hw,
	[CLKID_SYS_TS_VPU]		= &sys_ts_vpu.hw,
	[CLKID_SYS_TS_HEVC]		= &sys_ts_hevc.hw,
	[CLKID_SYS_PWM_AB]		= &sys_pwm_ab.hw,
	[CLKID_SYS_PWM_CD]		= &sys_pwm_cd.hw,
	[CLKID_SYS_PWM_EF]		= &sys_pwm_ef.hw,
	[CLKID_SYS_PWM_AO_AB]		= &sys_pwm_ao_ab.hw,
	[CLKID_SYS_PWM_AO_CD]		= &sys_pwm_ao_cd.hw,
	[CLKID_SYS_PWM_AO_EF]		= &sys_pwm_ao_ef.hw,
	[CLKID_SYS_PWM_AO_GH]		= &sys_pwm_ao_gh.hw,
};

/* Convenience table to populate regmap in .probe */
static struct clk_regmap *const t7_periphs_regmaps[] = {
	&rtc_32k_in,
	&rtc_32k_div,
	&rtc_32k_force_sel,
	&rtc_32k_out,
	&rtc_32k_mux0_0,
	&rtc_32k_mux0_1,
	&rtc,
	&cecb_32k_in,
	&cecb_32k_div,
	&cecb_32k_sel_pre,
	&cecb_32k_sel,
	&ceca_32k_in,
	&ceca_32k_div,
	&ceca_32k_sel_pre,
	&ceca_32k_sel,
	&ceca_32k_out,
	&cecb_32k_out,
	&sc_sel,
	&sc_div,
	&sc,
	&dspa_a_sel,
	&dspa_a_div,
	&dspa_a,
	&dspa_b_sel,
	&dspa_b_div,
	&dspa_b,
	&dspa,
	&dspb_a_sel,
	&dspb_a_div,
	&dspb_a,
	&dspb_b_sel,
	&dspb_b_div,
	&dspb_b,
	&dspb,
	&clk_24m,
	&clk_12m,
	&anakin_0_sel,
	&anakin_0_div,
	&anakin_0,
	&anakin_1_sel,
	&anakin_1_div,
	&anakin_1,
	&anakin,
	&anakin_clk,
	&fdiv2_divn_pre,
	&fdiv2_divn,
	&mipi_csi_phy0_sel,
	&mipi_csi_phy0_div,
	&mipi_csi_phy0,
	&mipi_csi_phy1_sel,
	&mipi_csi_phy1_div,
	&mipi_csi_phy1,
	&mipi_csi_phy,
	&mipi_isp_sel,
	&mipi_isp_div,
	&mipi_isp,
	&ts_div,
	&ts,
	&mali_0_sel,
	&mali_0_div,
	&mali_0,
	&mali_1_sel,
	&mali_1_div,
	&mali_1,
	&mali,
	&eth_rmii_sel,
	&eth_rmii_div,
	&eth_rmii,
	&eth_125m,
	&sd_emmc_c_sel,
	&sd_emmc_c_div,
	&sd_emmc_c,
	&sd_emmc_a_sel,
	&sd_emmc_a_div,
	&sd_emmc_a,
	&sd_emmc_b_sel,
	&sd_emmc_b_div,
	&sd_emmc_b,
	&spicc0_sel,
	&spicc0_div,
	&spicc0,
	&spicc1_sel,
	&spicc1_div,
	&spicc1,
	&spicc2_sel,
	&spicc2_div,
	&spicc2,
	&spicc3_sel,
	&spicc3_div,
	&spicc3,
	&spicc4_sel,
	&spicc4_div,
	&spicc4,
	&spicc5_sel,
	&spicc5_div,
	&spicc5,
	&saradc_sel,
	&saradc_div,
	&saradc,
	&pwm_a_sel,
	&pwm_a_div,
	&pwm_a,
	&pwm_b_sel,
	&pwm_b_div,
	&pwm_b,
	&pwm_c_sel,
	&pwm_c_div,
	&pwm_c,
	&pwm_d_sel,
	&pwm_d_div,
	&pwm_d,
	&pwm_e_sel,
	&pwm_e_div,
	&pwm_e,
	&pwm_f_sel,
	&pwm_f_div,
	&pwm_f,
	&pwm_ao_a_sel,
	&pwm_ao_a_div,
	&pwm_ao_a,
	&pwm_ao_b_sel,
	&pwm_ao_b_div,
	&pwm_ao_b,
	&pwm_ao_c_sel,
	&pwm_ao_c_div,
	&pwm_ao_c,
	&pwm_ao_d_sel,
	&pwm_ao_d_div,
	&pwm_ao_d,
	&pwm_ao_e_sel,
	&pwm_ao_e_div,
	&pwm_ao_e,
	&pwm_ao_f_sel,
	&pwm_ao_f_div,
	&pwm_ao_f,
	&pwm_ao_g_sel,
	&pwm_ao_g_div,
	&pwm_ao_g,
	&pwm_ao_h_sel,
	&pwm_ao_h_div,
	&pwm_ao_h,
	&pwm_ao_h,
	&sys_ddr,
	&sys_dos,
	&sys_mipi_dsi_a,
	&sys_mipi_dsi_b,
	&sys_ethphy,
	&sys_mali,
	&sys_aocpu,
	&sys_aucpu,
	&sys_cec,
	&sys_gdc,
	&sys_deswarp,
	&sys_ampipe_nand,
	&sys_ampipe_eth,
	&sys_am2axi0,
	&sys_am2axi1,
	&sys_am2axi2,
	&sys_sdemmca,
	&sys_sdemmcb,
	&sys_sdemmcc,
	&sys_smartcard,
	&sys_acodec,
	&sys_spifc,
	&sys_msr_clk,
	&sys_ir_ctrl,
	&sys_audio,
	&sys_eth,
	&sys_uart_a,
	&sys_uart_b,
	&sys_uart_c,
	&sys_uart_d,
	&sys_uart_e,
	&sys_uart_f,
	&sys_aififo,
	&sys_spicc2,
	&sys_spicc3,
	&sys_spicc4,
	&sys_ts_a73,
	&sys_ts_a53,
	&sys_spicc5,
	&sys_g2d,
	&sys_spicc0,
	&sys_spicc1,
	&sys_pcie,
	&sys_usb,
	&sys_pcie_phy,
	&sys_i2c_ao_a,
	&sys_i2c_ao_b,
	&sys_i2c_m_a,
	&sys_i2c_m_b,
	&sys_i2c_m_c,
	&sys_i2c_m_d,
	&sys_i2c_m_e,
	&sys_i2c_m_f,
	&sys_hdmitx_apb,
	&sys_i2c_s_a,
	&sys_hdmirx_pclk,
	&sys_mmc_apb,
	&sys_mipi_isp_pclk,
	&sys_rsa,
	&sys_pclk_sys_apb,
	&sys_a73pclk_apb,
	&sys_dspa,
	&sys_dspb,
	&sys_vpu_intr,
	&sys_sar_adc,
	&sys_gic,
	&sys_ts_gpu,
	&sys_ts_nna,
	&sys_ts_vpu,
	&sys_ts_hevc,
	&sys_pwm_ab,
	&sys_pwm_cd,
	&sys_pwm_ef,
	&sys_pwm_ao_ab,
	&sys_pwm_ao_cd,
	&sys_pwm_ao_ef,
	&sys_pwm_ao_gh,
};

static const struct regmap_config t7_periphs_regmap_cfg = {
	.reg_bits   = 32,
	.val_bits   = 32,
	.reg_stride = 4,
	.max_register = CLKCTRL_SPICC_CLK_CTRL2
};

static struct meson_clk_hw_data t7_periphs_clks = {
	.hws = t7_periphs_hw_clks,
	.num = ARRAY_SIZE(t7_periphs_hw_clks),
};

static int amlogic_t7_periphs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *base;
	struct regmap *map;
	int i, ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	map = devm_regmap_init_mmio(dev, base, &t7_periphs_regmap_cfg);
	if (IS_ERR(map))
		return PTR_ERR(map);

	/* Populate regmap for the regmap backed clocks */
	for (i = 0; i < ARRAY_SIZE(t7_periphs_regmaps); i++)
		t7_periphs_regmaps[i]->map = map;

	for (i = 0; i < t7_periphs_clks.num; i++) {
		ret = devm_clk_hw_register(dev, t7_periphs_clks.hws[i]);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(dev, meson_clk_hw_get, &t7_periphs_clks);
}

static const struct of_device_id t7_periphs_clkc_match_table[] = {
	{ .compatible = "amlogic,t7-peripherals-clkc", },
	{}
};
MODULE_DEVICE_TABLE(of, t7_periphs_clkc_match_table);

static struct platform_driver t7_periphs_clkc_driver = {
	.probe = amlogic_t7_periphs_probe,
	.driver = {
		.name = "t7-peripherals-clkc",
		.of_match_table = t7_periphs_clkc_match_table,
	},
};

MODULE_DESCRIPTION("Amlogic T7 Peripherals Clock Controller driver");
module_platform_driver(t7_periphs_clkc_driver);
MODULE_AUTHOR("Jian Hu <jian.hu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CLK_MESON);
