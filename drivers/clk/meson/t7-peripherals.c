// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2024 Amlogic, Inc. All rights reserved.
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

static struct clk_regmap t7_rtc_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_rtc_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static const struct meson_clk_dualdiv_param t7_clk_32k_div_table[] = {
	{
		.n1	= 733, .m1	= 8,
		.n2	= 732, .m2	= 11,
		.dual	= 1,
	},
	{}
};

static struct clk_regmap t7_rtc_32k_div = {
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
		.table = t7_clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_rtc_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_rtc_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_rtc_32k_force_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_rtc_32k_force_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_rtc_32k_div.hw,
			&t7_rtc_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_rtc_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_RTC_BY_OSCIN_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_rtc_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_rtc_32k_force_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_rtc_32k_mux0_0 = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_rtc_32k_mux0_0",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "xtal", },
			{ .hw = &t7_rtc_32k_out.hw },
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_rtc_32k_mux0_1 = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_rtc_32k_mux0_1",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "pad", },
			{ .fw_name = "xtal", },
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_rtc = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_RTC_CTRL,
		.mask = 0x1,
		.shift = 1,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_rtc",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_rtc_32k_mux0_0.hw,
			&t7_rtc_32k_mux0_1.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_ceca_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECA_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_ceca_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_ceca_32k_div = {
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
		.table = t7_clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ceca_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_ceca_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_ceca_32k_sel_pre = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECA_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ceca_32k_sel_pre",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_ceca_32k_div.hw,
			&t7_ceca_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_ceca_32k_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECA_CTRL1,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ceca_32k_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_ceca_32k_sel_pre.hw,
			&t7_rtc.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_ceca_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECA_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ceca_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_ceca_32k_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_cecb_32k_in = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECB_CTRL0,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_cecb_32k_in",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_cecb_32k_div = {
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
		.table = t7_clk_32k_div_table,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_cecb_32k_div",
		.ops = &meson_clk_dualdiv_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_cecb_32k_in.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_cecb_32k_sel_pre = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECB_CTRL1,
		.mask = 0x1,
		.shift = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_cecb_32k_sel_pre",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_cecb_32k_div.hw,
			&t7_cecb_32k_in.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_cecb_32k_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_CECB_CTRL1,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_cecb_32k_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_cecb_32k_sel_pre.hw,
			&t7_rtc.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_cecb_32k_out = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CECB_CTRL0,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_cecb_32k_out",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_cecb_32k_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/* Smartcard Clock */
static const struct clk_parent_data t7_sc_parents[] = {
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "xtal", },
};

static struct clk_regmap t7_sc_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.mask = 0x3,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sc_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_sc_parents,
		.num_parents = ARRAY_SIZE(t7_sc_parents),
	},
};

static struct clk_regmap t7_sc_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sc_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sc_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sc = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SC_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sc",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sc_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The DSPA/B IP is clocked by two identical clocks (dspa/b_a and dspa/b_b)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data t7_dsp_ab_parent_data[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "hifi", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv7", },
	{ .hw = &t7_rtc.hw },
};

static struct clk_regmap t7_dspa_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x7,
		.shift = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspa_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(t7_dsp_ab_parent_data),
	},
};

static struct clk_regmap t7_dspa_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.shift = 0,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspa_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspa_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspa_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.bit_idx = 13,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_dspa_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspa_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspa_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x7,
		.shift = 26,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspa_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(t7_dsp_ab_parent_data),
	},
};

static struct clk_regmap t7_dspa_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.shift = 16,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspa_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspa_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspa_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.bit_idx = 29,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_dspa_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspa_b_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspa = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPA_CLK_CTRL0,
		.mask = 0x1,
		.shift = 15,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspa",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspa_a.hw,
			&t7_dspa_b.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspb_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x7,
		.shift = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspb_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(t7_dsp_ab_parent_data),
	},
};

static struct clk_regmap t7_dspb_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.shift = 0,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspb_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspb_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspb_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.bit_idx = 13,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_dspb_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspb_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspb_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x7,
		.shift = 26,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspb_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_dsp_ab_parent_data,
		.num_parents = ARRAY_SIZE(t7_dsp_ab_parent_data),
	},
};

static struct clk_regmap t7_dspb_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.shift = 16,
		.width = 10,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspb_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspb_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspb_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.bit_idx = 29,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_dspb_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspb_b_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_dspb = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_DSPB_CLK_CTRL0,
		.mask = 0x1,
		.shift = 15,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_dspb",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_dspb_a.hw,
			&t7_dspb_b.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_clk_24m = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 11,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_24m",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_fixed_factor t7_clk_24m_div2 = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "t7_24m_div2",
		.ops = &clk_fixed_factor_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_clk_24m.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_clk_12m = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 10,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_12m",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_clk_24m_div2.hw
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fdiv2_divn_pre = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_fdiv2_divn_pre",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "fdiv2",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_fdiv2_divn = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_CLK12_24_CTRL,
		.bit_idx = 12,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_fdiv2_divn",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fdiv2_divn_pre.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The NNA IP is clocked by two identical clocks (anakin_0 and anakin_1)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data t7_anakin_parent_data[] = {
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "vid_pll0", },
	{ .fw_name = "mpll1", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "fdiv2p5", },
};

static struct clk_regmap t7_anakin_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_anakin_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_anakin_parent_data,
		.num_parents = ARRAY_SIZE(t7_anakin_parent_data),
	},
};

static struct clk_regmap t7_anakin_0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_anakin_0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_anakin_0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_anakin_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_anakin_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) { &t7_anakin_0_div.hw },
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_anakin_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_anakin_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_anakin_parent_data,
		.num_parents = ARRAY_SIZE(t7_anakin_parent_data),
	},
};

static struct clk_regmap t7_anakin_1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_anakin_1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_anakin_1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_anakin_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_anakin_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_anakin_1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_anakin = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.mask = 1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_anakin_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_anakin_0.hw,
			&t7_anakin_1.hw
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT
	},
};

static struct clk_regmap t7_anakin_clk = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_ANAKIN_CLK_CTRL,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_anakin_clk",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_anakin.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT
	},
};

static const struct clk_parent_data t7_mipi_csi_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "gp1", },
	{ .fw_name = "mpll1", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
};

static struct clk_regmap t7_mipi_csi_phy0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_csi_phy0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_mipi_csi_parents,
		.num_parents = ARRAY_SIZE(t7_mipi_csi_parents),
	},
};

static struct clk_regmap t7_mipi_csi_phy0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_csi_phy0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_csi_phy0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mipi_csi_phy0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_mipi_csi_phy0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_csi_phy0_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mipi_csi_phy1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_csi_phy1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_mipi_csi_parents,
		.num_parents = ARRAY_SIZE(t7_mipi_csi_parents),
	},
};

static struct clk_regmap t7_mipi_csi_phy1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_csi_phy1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_csi_phy1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mipi_csi_phy1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_mipi_csi_phy1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_csi_phy1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mipi_csi_phy = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_CSI_PHY_CLK_CTRL,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_csi_phy",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_csi_phy0.hw,
			&t7_mipi_csi_phy1.hw
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data t7_mipi_isp_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "mpll3", },
	{ .fw_name = "gp1", },
};

static struct clk_regmap t7_mipi_isp_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_isp_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_mipi_isp_parents,
		.num_parents = ARRAY_SIZE(t7_mipi_isp_parents),
	},
};

static struct clk_regmap t7_mipi_isp_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mipi_isp_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_isp_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mipi_isp = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MIPI_ISP_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_mipi_isp",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mipi_isp_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_ts_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_TS_CLK_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ts_div",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "xtal",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_ts = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_TS_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_ts",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_ts_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/*
 * The MALI IP is clocked by two identical clocks (mali_0 and mali_1)
 * muxed by a glitch-free switch.
 */
static const struct clk_parent_data t7_mali_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "gp0", },
	{ .fw_name = "gp1", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
};

static struct clk_regmap t7_mali_0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_mali_parents,
		.num_parents = ARRAY_SIZE(t7_mali_parents),
	},
};

static struct clk_regmap t7_mali_0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mali_0_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mali_0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_0",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mali_0_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mali_1_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_1_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_mali_parents,
		.num_parents = ARRAY_SIZE(t7_mali_parents),
	},
};

static struct clk_regmap t7_mali_1_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_1_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mali_1_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mali_1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali_1",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mali_1_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_GATE | CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_mali = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_MALI_CLK_CTRL,
		.mask = 1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_mali",
		.ops = &clk_regmap_mux_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_mali_0.hw,
			&t7_mali_1.hw,
		},
		.num_parents = 2,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static u32 t7_eth_rmii_table[] = { 0, 7 };

static const struct clk_parent_data t7_eth_rmii_parents[] = {
	{ .fw_name = "fdiv2", },
	{ .fw_name = "rmii_pad", },
};

static struct clk_regmap t7_eth_rmii_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.mask = 0x3,
		.shift = 9,
		.table = t7_eth_rmii_table
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_eth_rmii_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_eth_rmii_parents,
		.num_parents = ARRAY_SIZE(t7_eth_rmii_parents),
	},
};

static struct clk_regmap t7_eth_rmii_div = {
	.data = &(struct clk_regmap_div_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_eth_rmii_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_eth_rmii_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_eth_rmii = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_eth_rmii",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_eth_rmii_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_fixed_factor t7_fdiv2_div8 = {
	.mult = 1,
	.div = 8,
	.hw.init = &(struct clk_init_data){
		.name = "t7_fdiv2_div8",
		.ops = &clk_fixed_factor_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "fdiv2",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap t7_eth_125m = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_ETH_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_eth_125m",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_fdiv2_div8.hw
		},
		.num_parents = 1,
	},
};

static const struct clk_parent_data t7_sd_emmc_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "hifi", },
	{ .fw_name = "fdiv2p5", },
	{ .fw_name = "mpll2", },
	{ .fw_name = "mpll3", },
	{ .fw_name = "gp0", },
};

static struct clk_regmap t7_sd_emmc_c_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_c_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_sd_emmc_parents,
		.num_parents = ARRAY_SIZE(t7_sd_emmc_parents),
	},
};

static struct clk_regmap t7_sd_emmc_c_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_c_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_c_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sd_emmc_c = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_NAND_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sd_emmc_c",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_c_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sd_emmc_a_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.mask = 0x7,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_a_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_sd_emmc_parents,
		.num_parents = ARRAY_SIZE(t7_sd_emmc_parents),
	},
};

static struct clk_regmap t7_sd_emmc_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_a_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sd_emmc_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sd_emmc_a",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_a_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sd_emmc_b_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.mask = 0x7,
		.shift = 25,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_b_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = t7_sd_emmc_parents,
		.num_parents = ARRAY_SIZE(t7_sd_emmc_parents),
	},
};

static struct clk_regmap t7_sd_emmc_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.shift = 16,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "t7_sd_emmc_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_b_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_sd_emmc_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = CLKCTRL_SD_EMMC_CLK_CTRL,
		.bit_idx = 23,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sd_emmc_b",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_sd_emmc_b_div.hw
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

static const struct clk_parent_data t7_spicc_parents[] = {
	{ .fw_name = "xtal", },
	{ .fw_name = "sys", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
	{ .fw_name = "fdiv2", },
	{ .fw_name = "fdiv5", },
	{ .fw_name = "fdiv7", },
	{ .fw_name = "gp1", },
};

static struct clk_regmap t7_spicc0_sel =
	SPI_PWM_CLK_MUX(t7_spicc0, CLKCTRL_SPICC_CLK_CTRL, 0x7, 7, t7_spicc_parents);
static struct clk_regmap t7_spicc0_div =
	SPI_PWM_CLK_DIV(t7_spicc0, CLKCTRL_SPICC_CLK_CTRL, 0, 6, t7_spicc0_sel);
static struct clk_regmap t7_spicc0 =
	SPI_PWM_CLK_GATE(t7_spicc0, CLKCTRL_SPICC_CLK_CTRL, 6, t7_spicc0_div);

static struct clk_regmap t7_spicc1_sel =
	SPI_PWM_CLK_MUX(t7_spicc1, CLKCTRL_SPICC_CLK_CTRL, 0x7, 23, t7_spicc_parents);
static struct clk_regmap t7_spicc1_div =
	SPI_PWM_CLK_DIV(t7_spicc1, CLKCTRL_SPICC_CLK_CTRL, 16, 6, t7_spicc1_sel);
static struct clk_regmap t7_spicc1 =
	SPI_PWM_CLK_GATE(t7_spicc1, CLKCTRL_SPICC_CLK_CTRL, 22, t7_spicc1_div);

static struct clk_regmap t7_spicc2_sel =
	SPI_PWM_CLK_MUX(t7_spicc2, CLKCTRL_SPICC_CLK_CTRL1, 0x7, 7, t7_spicc_parents);
static struct clk_regmap t7_spicc2_div =
	SPI_PWM_CLK_DIV(t7_spicc2, CLKCTRL_SPICC_CLK_CTRL1, 0, 6, t7_spicc2_sel);
static struct clk_regmap t7_spicc2 =
	SPI_PWM_CLK_GATE(t7_spicc2, CLKCTRL_SPICC_CLK_CTRL1, 6, t7_spicc2_div);

static struct clk_regmap t7_spicc3_sel =
	SPI_PWM_CLK_MUX(t7_spicc3, CLKCTRL_SPICC_CLK_CTRL1, 0x7, 23, t7_spicc_parents);
static struct clk_regmap t7_spicc3_div =
	SPI_PWM_CLK_DIV(t7_spicc3, CLKCTRL_SPICC_CLK_CTRL1, 16, 6, t7_spicc3_sel);
static struct clk_regmap t7_spicc3 =
	SPI_PWM_CLK_GATE(t7_spicc3, CLKCTRL_SPICC_CLK_CTRL1, 22, t7_spicc3_div);

static struct clk_regmap t7_spicc4_sel =
	SPI_PWM_CLK_MUX(t7_spicc4, CLKCTRL_SPICC_CLK_CTRL2, 0x7, 7, t7_spicc_parents);
static struct clk_regmap t7_spicc4_div =
	SPI_PWM_CLK_DIV(t7_spicc4, CLKCTRL_SPICC_CLK_CTRL2, 0, 6, t7_spicc4_sel);
static struct clk_regmap t7_spicc4 =
	SPI_PWM_CLK_GATE(t7_spicc4, CLKCTRL_SPICC_CLK_CTRL2, 6, t7_spicc4_div);

static struct clk_regmap t7_spicc5_sel =
	SPI_PWM_CLK_MUX(t7_spicc5, CLKCTRL_SPICC_CLK_CTRL2, 0x7, 23, t7_spicc_parents);
static struct clk_regmap t7_spicc5_div =
	SPI_PWM_CLK_DIV(t7_spicc5, CLKCTRL_SPICC_CLK_CTRL2, 16, 6, t7_spicc5_sel);
static struct clk_regmap t7_spicc5 =
	SPI_PWM_CLK_GATE(t7_spicc5, CLKCTRL_SPICC_CLK_CTRL2, 22, t7_spicc5_div);

static struct clk_regmap t7_saradc_sel = {
	.data = &(struct clk_regmap_mux_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.mask = 0x1,
		.shift = 9,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_saradc_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_data = (const struct clk_parent_data []) {
			{ .fw_name = "xtal", },
			{ .fw_name = "sys", },
		},
		.num_parents = 2,
	},
};

static struct clk_regmap t7_saradc_div = {
	.data = &(struct clk_regmap_div_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_saradc_div",
		.ops = &clk_regmap_divider_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_saradc_sel.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap t7_saradc = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_SAR_CLK_CTRL0,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_saradc",
		.ops = &clk_regmap_gate_ops,
		.parent_hws = (const struct clk_hw *[]) {
			&t7_saradc_div.hw
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data t7_pwm_parents[]  = {
	{ .fw_name = "xtal", },
	{ .fw_name = "vid_pll0", },
	{ .fw_name = "fdiv4", },
	{ .fw_name = "fdiv3", },
};

static struct clk_regmap t7_pwm_a_sel =
	SPI_PWM_CLK_MUX(t7_pwm, CLKCTRL_PWM_CLK_AB_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_a_div =
	SPI_PWM_CLK_DIV(t7_pwm_a, CLKCTRL_PWM_CLK_AB_CTRL, 0, 8, t7_pwm_a_sel);
static struct clk_regmap t7_pwm_a =
	SPI_PWM_CLK_GATE(t7_pwm_a, CLKCTRL_PWM_CLK_AB_CTRL, 8, t7_pwm_a_div);

static struct clk_regmap t7_pwm_b_sel =
	SPI_PWM_CLK_MUX(t7_pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_b_div =
	SPI_PWM_CLK_DIV(t7_pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 16, 8, t7_pwm_b_sel);
static struct clk_regmap t7_pwm_b =
	SPI_PWM_CLK_GATE(t7_pwm_b, CLKCTRL_PWM_CLK_AB_CTRL, 24, t7_pwm_b_div);

static struct clk_regmap t7_pwm_c_sel =
	SPI_PWM_CLK_MUX(t7_pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_c_div =
	SPI_PWM_CLK_DIV(t7_pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 0, 8, t7_pwm_c_sel);
static struct clk_regmap t7_pwm_c =
	SPI_PWM_CLK_GATE(t7_pwm_c, CLKCTRL_PWM_CLK_CD_CTRL, 8, t7_pwm_c_div);

static struct clk_regmap t7_pwm_d_sel =
	SPI_PWM_CLK_MUX(t7_pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_d_div =
	SPI_PWM_CLK_DIV(t7_pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 16, 8, t7_pwm_d_sel);
static struct clk_regmap t7_pwm_d =
	SPI_PWM_CLK_GATE(t7_pwm_d, CLKCTRL_PWM_CLK_CD_CTRL, 24, t7_pwm_d_div);

static struct clk_regmap t7_pwm_e_sel =
	SPI_PWM_CLK_MUX(t7_pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_e_div =
	SPI_PWM_CLK_DIV(t7_pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 0, 8, t7_pwm_e_sel);
static struct clk_regmap t7_pwm_e =
	SPI_PWM_CLK_GATE(t7_pwm_e, CLKCTRL_PWM_CLK_EF_CTRL, 8, t7_pwm_e_div);

static struct clk_regmap t7_pwm_f_sel =
	SPI_PWM_CLK_MUX(t7_pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_f_div =
	SPI_PWM_CLK_DIV(t7_pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 16, 8, t7_pwm_f_sel);
static struct clk_regmap t7_pwm_f =
	SPI_PWM_CLK_GATE(t7_pwm_f, CLKCTRL_PWM_CLK_EF_CTRL, 24, t7_pwm_f_div);

static struct clk_regmap t7_pwm_ao_a_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_a_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0, 8, t7_pwm_ao_a_sel);
static struct clk_regmap t7_pwm_ao_a =
	SPI_PWM_CLK_GATE(t7_pwm_ao_a, CLKCTRL_PWM_CLK_AO_AB_CTRL, 8, t7_pwm_ao_a_div);

static struct clk_regmap t7_pwm_ao_b_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_b_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 16, 8, t7_pwm_ao_b_sel);
static struct clk_regmap t7_pwm_ao_b =
	SPI_PWM_CLK_GATE(t7_pwm_ao_b, CLKCTRL_PWM_CLK_AO_AB_CTRL, 24, t7_pwm_ao_b_div);

static struct clk_regmap t7_pwm_ao_c_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_c_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0, 8, t7_pwm_ao_c_sel);
static struct clk_regmap t7_pwm_ao_c =
	SPI_PWM_CLK_GATE(t7_pwm_ao_c, CLKCTRL_PWM_CLK_AO_CD_CTRL, 8, t7_pwm_ao_c_div);

static struct clk_regmap t7_pwm_ao_d_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_d_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 16, 8, t7_pwm_ao_d_sel);
static struct clk_regmap t7_pwm_ao_d =
	SPI_PWM_CLK_GATE(t7_pwm_ao_d, CLKCTRL_PWM_CLK_AO_CD_CTRL, 24, t7_pwm_ao_d_div);

static struct clk_regmap t7_pwm_ao_e_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_e_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0, 8, t7_pwm_ao_e_sel);
static struct clk_regmap t7_pwm_ao_e =
	SPI_PWM_CLK_GATE(t7_pwm_ao_e, CLKCTRL_PWM_CLK_AO_EF_CTRL, 8, t7_pwm_ao_e_div);

static struct clk_regmap t7_pwm_ao_f_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_f_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 16, 8, t7_pwm_ao_f_sel);
static struct clk_regmap t7_pwm_ao_f =
	SPI_PWM_CLK_GATE(t7_pwm_ao_f, CLKCTRL_PWM_CLK_AO_EF_CTRL, 24, t7_pwm_ao_f_div);

static struct clk_regmap t7_pwm_ao_g_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0x3, 9, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_g_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0, 8, t7_pwm_ao_g_sel);
static struct clk_regmap t7_pwm_ao_g =
	SPI_PWM_CLK_GATE(t7_pwm_ao_g, CLKCTRL_PWM_CLK_AO_GH_CTRL, 8, t7_pwm_ao_g_div);

static struct clk_regmap t7_pwm_ao_h_sel =
	SPI_PWM_CLK_MUX(t7_pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 0x3, 25, t7_pwm_parents);
static struct clk_regmap t7_pwm_ao_h_div =
	SPI_PWM_CLK_DIV(t7_pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 16, 8, t7_pwm_ao_h_sel);
static struct clk_regmap t7_pwm_ao_h =
	SPI_PWM_CLK_GATE(t7_pwm_ao_h, CLKCTRL_PWM_CLK_AO_GH_CTRL, 24, t7_pwm_ao_h_div);

#define MESON_GATE(_name, _reg, _bit)  MESON_PCLK_V2(_name, _reg, _bit, sys)

static MESON_GATE(t7_sys_ddr,			CLKCTRL_SYS_CLK_EN0_REG0, 0);
static MESON_GATE(t7_sys_dos,			CLKCTRL_SYS_CLK_EN0_REG0, 1);
static MESON_GATE(t7_sys_mipi_dsi_a,		CLKCTRL_SYS_CLK_EN0_REG0, 2);
static MESON_GATE(t7_sys_mipi_dsi_b,		CLKCTRL_SYS_CLK_EN0_REG0, 3);
static MESON_GATE(t7_sys_ethphy,		CLKCTRL_SYS_CLK_EN0_REG0, 4);
static MESON_GATE(t7_sys_mali,			CLKCTRL_SYS_CLK_EN0_REG0, 6);
static MESON_GATE(t7_sys_aocpu,			CLKCTRL_SYS_CLK_EN0_REG0, 13);
static MESON_GATE(t7_sys_aucpu,			CLKCTRL_SYS_CLK_EN0_REG0, 14);
static MESON_GATE(t7_sys_cec,			CLKCTRL_SYS_CLK_EN0_REG0, 16);
static MESON_GATE(t7_sys_gdc,			CLKCTRL_SYS_CLK_EN0_REG0, 17);
static MESON_GATE(t7_sys_deswarp,		CLKCTRL_SYS_CLK_EN0_REG0, 18);
static MESON_GATE(t7_sys_ampipe_nand,		CLKCTRL_SYS_CLK_EN0_REG0, 19);
static MESON_GATE(t7_sys_ampipe_eth,		CLKCTRL_SYS_CLK_EN0_REG0, 20);
static MESON_GATE(t7_sys_am2axi0,		CLKCTRL_SYS_CLK_EN0_REG0, 21);
static MESON_GATE(t7_sys_am2axi1,		CLKCTRL_SYS_CLK_EN0_REG0, 22);
static MESON_GATE(t7_sys_am2axi2,		CLKCTRL_SYS_CLK_EN0_REG0, 23);
static MESON_GATE(t7_sys_sdemmca,		CLKCTRL_SYS_CLK_EN0_REG0, 24);
static MESON_GATE(t7_sys_sdemmcb,		CLKCTRL_SYS_CLK_EN0_REG0, 25);
static MESON_GATE(t7_sys_sdemmcc,		CLKCTRL_SYS_CLK_EN0_REG0, 26);
static MESON_GATE(t7_sys_smartcard,		CLKCTRL_SYS_CLK_EN0_REG0, 27);
static MESON_GATE(t7_sys_acodec,		CLKCTRL_SYS_CLK_EN0_REG0, 28);
static MESON_GATE(t7_sys_spifc,			CLKCTRL_SYS_CLK_EN0_REG0, 29);
static MESON_GATE(t7_sys_msr_clk,		CLKCTRL_SYS_CLK_EN0_REG0, 30);
static MESON_GATE(t7_sys_ir_ctrl,		CLKCTRL_SYS_CLK_EN0_REG0, 31);
static MESON_GATE(t7_sys_audio,			CLKCTRL_SYS_CLK_EN0_REG1, 0);
static MESON_GATE(t7_sys_eth,			CLKCTRL_SYS_CLK_EN0_REG1, 3);
static MESON_GATE(t7_sys_uart_a,		CLKCTRL_SYS_CLK_EN0_REG1, 5);
static MESON_GATE(t7_sys_uart_b,		CLKCTRL_SYS_CLK_EN0_REG1, 6);
static MESON_GATE(t7_sys_uart_c,		CLKCTRL_SYS_CLK_EN0_REG1, 7);
static MESON_GATE(t7_sys_uart_d,		CLKCTRL_SYS_CLK_EN0_REG1, 8);
static MESON_GATE(t7_sys_uart_e,		CLKCTRL_SYS_CLK_EN0_REG1, 9);
static MESON_GATE(t7_sys_uart_f,		CLKCTRL_SYS_CLK_EN0_REG1, 10);
static MESON_GATE(t7_sys_aififo,		CLKCTRL_SYS_CLK_EN0_REG1, 11);
static MESON_GATE(t7_sys_spicc2,		CLKCTRL_SYS_CLK_EN0_REG1, 12);
static MESON_GATE(t7_sys_spicc3,		CLKCTRL_SYS_CLK_EN0_REG1, 13);
static MESON_GATE(t7_sys_spicc4,		CLKCTRL_SYS_CLK_EN0_REG1, 14);
static MESON_GATE(t7_sys_ts_a73,		CLKCTRL_SYS_CLK_EN0_REG1, 15);
static MESON_GATE(t7_sys_ts_a53,		CLKCTRL_SYS_CLK_EN0_REG1, 16);
static MESON_GATE(t7_sys_spicc5,		CLKCTRL_SYS_CLK_EN0_REG1, 17);
static MESON_GATE(t7_sys_g2d,			CLKCTRL_SYS_CLK_EN0_REG1, 20);
static MESON_GATE(t7_sys_spicc0,		CLKCTRL_SYS_CLK_EN0_REG1, 21);
static MESON_GATE(t7_sys_spicc1,		CLKCTRL_SYS_CLK_EN0_REG1, 22);
static MESON_GATE(t7_sys_pcie,			CLKCTRL_SYS_CLK_EN0_REG1, 24);
static MESON_GATE(t7_sys_usb,			CLKCTRL_SYS_CLK_EN0_REG1, 26);
static MESON_GATE(t7_sys_pcie_phy,		CLKCTRL_SYS_CLK_EN0_REG1, 27);
static MESON_GATE(t7_sys_i2c_ao_a,		CLKCTRL_SYS_CLK_EN0_REG1, 28);
static MESON_GATE(t7_sys_i2c_ao_b,		CLKCTRL_SYS_CLK_EN0_REG1, 29);
static MESON_GATE(t7_sys_i2c_m_a,		CLKCTRL_SYS_CLK_EN0_REG1, 30);
static MESON_GATE(t7_sys_i2c_m_b,		CLKCTRL_SYS_CLK_EN0_REG1, 31);
static MESON_GATE(t7_sys_i2c_m_c,		CLKCTRL_SYS_CLK_EN0_REG2, 0);
static MESON_GATE(t7_sys_i2c_m_d,		CLKCTRL_SYS_CLK_EN0_REG2, 1);
static MESON_GATE(t7_sys_i2c_m_e,		CLKCTRL_SYS_CLK_EN0_REG2, 2);
static MESON_GATE(t7_sys_i2c_m_f,		CLKCTRL_SYS_CLK_EN0_REG2, 3);
static MESON_GATE(t7_sys_hdmitx_apb,		CLKCTRL_SYS_CLK_EN0_REG2, 4);
static MESON_GATE(t7_sys_i2c_s_a,		CLKCTRL_SYS_CLK_EN0_REG2, 5);
static MESON_GATE(t7_sys_hdmirx_pclk,		CLKCTRL_SYS_CLK_EN0_REG2, 8);
static MESON_GATE(t7_sys_mmc_apb,		CLKCTRL_SYS_CLK_EN0_REG2, 11);
static MESON_GATE(t7_sys_mipi_isp_pclk,		CLKCTRL_SYS_CLK_EN0_REG2, 17);
static MESON_GATE(t7_sys_rsa,			CLKCTRL_SYS_CLK_EN0_REG2, 18);
static MESON_GATE(t7_sys_pclk_sys_apb,		CLKCTRL_SYS_CLK_EN0_REG2, 19);
static MESON_GATE(t7_sys_a73pclk_apb,		CLKCTRL_SYS_CLK_EN0_REG2, 20);
static MESON_GATE(t7_sys_dspa,			CLKCTRL_SYS_CLK_EN0_REG2, 21);
static MESON_GATE(t7_sys_dspb,			CLKCTRL_SYS_CLK_EN0_REG2, 22);
static MESON_GATE(t7_sys_vpu_intr,		CLKCTRL_SYS_CLK_EN0_REG2, 25);
static MESON_GATE(t7_sys_sar_adc,		CLKCTRL_SYS_CLK_EN0_REG2, 28);
static MESON_GATE(t7_sys_ts_gpu,		CLKCTRL_SYS_CLK_EN0_REG2, 31);
static MESON_GATE(t7_sys_ts_nna,		CLKCTRL_SYS_CLK_EN0_REG3, 0);
static MESON_GATE(t7_sys_ts_vpu,		CLKCTRL_SYS_CLK_EN0_REG3, 1);
static MESON_GATE(t7_sys_ts_hevc,		CLKCTRL_SYS_CLK_EN0_REG3, 2);
static MESON_GATE(t7_sys_pwm_ao_ab,		CLKCTRL_SYS_CLK_EN0_REG3, 3);
static MESON_GATE(t7_sys_pwm_ao_cd,		CLKCTRL_SYS_CLK_EN0_REG3, 4);
static MESON_GATE(t7_sys_pwm_ao_ef,		CLKCTRL_SYS_CLK_EN0_REG3, 5);
static MESON_GATE(t7_sys_pwm_ao_gh,		CLKCTRL_SYS_CLK_EN0_REG3, 6);
static MESON_GATE(t7_sys_pwm_ab,		CLKCTRL_SYS_CLK_EN0_REG3, 7);
static MESON_GATE(t7_sys_pwm_cd,		CLKCTRL_SYS_CLK_EN0_REG3, 8);
static MESON_GATE(t7_sys_pwm_ef,		CLKCTRL_SYS_CLK_EN0_REG3, 9);

/*
 * sys_gic provides the clock for GIC(Generic Interrupt Controller).
 * After clock is disabled, The GIC cannot work properly. At present, the driver
 * used by our GIC is the public driver in kernel, and there is no management
 * clock in the driver.
 */
static struct clk_regmap t7_sys_gic = {
	.data = &(struct clk_regmap_gate_data) {
		.offset = CLKCTRL_SYS_CLK_EN0_REG2,
		.bit_idx = 30,
	},
	.hw.init = &(struct clk_init_data){
		.name = "t7_sys_gic",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "sys",
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT | CLK_IS_CRITICAL,
	},
};

/* Array of all clocks registered by this provider */
static struct clk_hw *t7_periphs_hw_clks[] = {
	[CLKID_RTC_32K_IN]		= &t7_rtc_32k_in.hw,
	[CLKID_RTC_32K_DIV]		= &t7_rtc_32k_div.hw,
	[CLKID_RTC_32K_FORCE_SEL]	= &t7_rtc_32k_force_sel.hw,
	[CLKID_RTC_32K_OUT]		= &t7_rtc_32k_out.hw,
	[CLKID_RTC_32K_MUX0_0]		= &t7_rtc_32k_mux0_0.hw,
	[CLKID_RTC_32K_MUX0_1]		= &t7_rtc_32k_mux0_1.hw,
	[CLKID_RTC]			= &t7_rtc.hw,
	[CLKID_CECB_32K_IN]		= &t7_cecb_32k_in.hw,
	[CLKID_CECB_32K_DIV]		= &t7_cecb_32k_div.hw,
	[CLKID_CECB_32K_SEL_PRE]	= &t7_cecb_32k_sel_pre.hw,
	[CLKID_CECB_32K_SEL]		= &t7_cecb_32k_sel.hw,
	[CLKID_CECA_32K_IN]		= &t7_ceca_32k_in.hw,
	[CLKID_CECA_32K_DIV]		= &t7_ceca_32k_div.hw,
	[CLKID_CECA_32K_SEL_PRE]	= &t7_ceca_32k_sel_pre.hw,
	[CLKID_CECA_32K_SEL]		= &t7_ceca_32k_sel.hw,
	[CLKID_CECA_32K]		= &t7_ceca_32k_out.hw,
	[CLKID_CECB_32K]		= &t7_cecb_32k_out.hw,
	[CLKID_SC_SEL]			= &t7_sc_sel.hw,
	[CLKID_SC_DIV]			= &t7_sc_div.hw,
	[CLKID_SC]			= &t7_sc.hw,
	[CLKID_DSPA_A_SEL]		= &t7_dspa_a_sel.hw,
	[CLKID_DSPA_A_DIV]		= &t7_dspa_a_div.hw,
	[CLKID_DSPA_A]			= &t7_dspa_a.hw,
	[CLKID_DSPA_B_SEL]		= &t7_dspa_b_sel.hw,
	[CLKID_DSPA_B_DIV]		= &t7_dspa_b_div.hw,
	[CLKID_DSPA_B]			= &t7_dspa_b.hw,
	[CLKID_DSPA]			= &t7_dspa.hw,
	[CLKID_DSPB_A_SEL]		= &t7_dspb_a_sel.hw,
	[CLKID_DSPB_A_DIV]		= &t7_dspb_a_div.hw,
	[CLKID_DSPB_A]			= &t7_dspb_a.hw,
	[CLKID_DSPB_B_SEL]		= &t7_dspb_b_sel.hw,
	[CLKID_DSPB_B_DIV]		= &t7_dspb_b_div.hw,
	[CLKID_DSPB_B]			= &t7_dspb_b.hw,
	[CLKID_DSPB]			= &t7_dspb.hw,
	[CLKID_CLK_24M]			= &t7_clk_24m.hw,
	[CLKID_CLK_24M_DIV2]		= &t7_clk_24m_div2.hw,
	[CLKID_CLK_12M]			= &t7_clk_12m.hw,
	[CLKID_ANAKIN_0_SEL]		= &t7_anakin_0_sel.hw,
	[CLKID_ANAKIN_0_DIV]		= &t7_anakin_0_div.hw,
	[CLKID_ANAKIN_0]		= &t7_anakin_0.hw,
	[CLKID_ANAKIN_1_SEL]		= &t7_anakin_1_sel.hw,
	[CLKID_ANAKIN_1_DIV]		= &t7_anakin_1_div.hw,
	[CLKID_ANAKIN_1]		= &t7_anakin_1.hw,
	[CLKID_ANAKIN]			= &t7_anakin.hw,
	[CLKID_ANAKIN_CLK]		= &t7_anakin_clk.hw,
	[CLKID_FCLK_DIV2_DIVN_PRE]	= &t7_fdiv2_divn_pre.hw,
	[CLKID_FCLK_DIV2_DIVN]		= &t7_fdiv2_divn.hw,
	[CLKID_MIPI_CSI_PHY_0_SEL]	= &t7_mipi_csi_phy0_sel.hw,
	[CLKID_MIPI_CSI_PHY_0_DIV]	= &t7_mipi_csi_phy0_div.hw,
	[CLKID_MIPI_CSI_PHY_0]		= &t7_mipi_csi_phy0.hw,
	[CLKID_MIPI_CSI_PHY_1_SEL]	= &t7_mipi_csi_phy1_sel.hw,
	[CLKID_MIPI_CSI_PHY_1_DIV]	= &t7_mipi_csi_phy1_div.hw,
	[CLKID_MIPI_CSI_PHY_1]		= &t7_mipi_csi_phy1.hw,
	[CLKID_MIPI_CSI_PHY]		= &t7_mipi_csi_phy.hw,
	[CLKID_MIPI_ISP_SEL]		= &t7_mipi_isp_sel.hw,
	[CLKID_MIPI_ISP_DIV]		= &t7_mipi_isp_div.hw,
	[CLKID_MIPI_ISP]		= &t7_mipi_isp.hw,
	[CLKID_TS_DIV]			= &t7_ts_div.hw,
	[CLKID_TS]			= &t7_ts.hw,
	[CLKID_MALI_0_SEL]		= &t7_mali_0_sel.hw,
	[CLKID_MALI_0_DIV]		= &t7_mali_0_div.hw,
	[CLKID_MALI_0]			= &t7_mali_0.hw,
	[CLKID_MALI_1_SEL]		= &t7_mali_1_sel.hw,
	[CLKID_MALI_1_DIV]		= &t7_mali_1_div.hw,
	[CLKID_MALI_1]			= &t7_mali_1.hw,
	[CLKID_MALI]			= &t7_mali.hw,
	[CLKID_ETH_RMII_SEL]		= &t7_eth_rmii_sel.hw,
	[CLKID_ETH_RMII_DIV]		= &t7_eth_rmii_div.hw,
	[CLKID_ETH_RMII]		= &t7_eth_rmii.hw,
	[CLKID_FCLK_DIV2_DIV8]		= &t7_fdiv2_div8.hw,
	[CLKID_ETH_125M]		= &t7_eth_125m.hw,
	[CLKID_SD_EMMC_C_SEL]		= &t7_sd_emmc_c_sel.hw,
	[CLKID_SD_EMMC_C_DIV]		= &t7_sd_emmc_c_div.hw,
	[CLKID_SD_EMMC_C]		= &t7_sd_emmc_c.hw,
	[CLKID_SD_EMMC_A_SEL]		= &t7_sd_emmc_a_sel.hw,
	[CLKID_SD_EMMC_A_DIV]		= &t7_sd_emmc_a_div.hw,
	[CLKID_SD_EMMC_A]		= &t7_sd_emmc_a.hw,
	[CLKID_SD_EMMC_B_SEL]		= &t7_sd_emmc_b_sel.hw,
	[CLKID_SD_EMMC_B_DIV]		= &t7_sd_emmc_b_div.hw,
	[CLKID_SD_EMMC_B]		= &t7_sd_emmc_b.hw,
	[CLKID_SPICC0_SEL]		= &t7_spicc0_sel.hw,
	[CLKID_SPICC0_DIV]		= &t7_spicc0_div.hw,
	[CLKID_SPICC0]			= &t7_spicc0.hw,
	[CLKID_SPICC1_SEL]		= &t7_spicc1_sel.hw,
	[CLKID_SPICC1_DIV]		= &t7_spicc1_div.hw,
	[CLKID_SPICC1]			= &t7_spicc1.hw,
	[CLKID_SPICC2_SEL]		= &t7_spicc2_sel.hw,
	[CLKID_SPICC2_DIV]		= &t7_spicc2_div.hw,
	[CLKID_SPICC2]			= &t7_spicc2.hw,
	[CLKID_SPICC3_SEL]		= &t7_spicc3_sel.hw,
	[CLKID_SPICC3_DIV]		= &t7_spicc3_div.hw,
	[CLKID_SPICC3]			= &t7_spicc3.hw,
	[CLKID_SPICC4_SEL]		= &t7_spicc4_sel.hw,
	[CLKID_SPICC4_DIV]		= &t7_spicc4_div.hw,
	[CLKID_SPICC4]			= &t7_spicc4.hw,
	[CLKID_SPICC5_SEL]		= &t7_spicc5_sel.hw,
	[CLKID_SPICC5_DIV]		= &t7_spicc5_div.hw,
	[CLKID_SPICC5]			= &t7_spicc5.hw,
	[CLKID_SARADC_SEL]		= &t7_saradc_sel.hw,
	[CLKID_SARADC_DIV]		= &t7_saradc_div.hw,
	[CLKID_SARADC]			= &t7_saradc.hw,
	[CLKID_PWM_A_SEL]		= &t7_pwm_a_sel.hw,
	[CLKID_PWM_A_DIV]		= &t7_pwm_a_div.hw,
	[CLKID_PWM_A]			= &t7_pwm_a.hw,
	[CLKID_PWM_B_SEL]		= &t7_pwm_b_sel.hw,
	[CLKID_PWM_B_DIV]		= &t7_pwm_b_div.hw,
	[CLKID_PWM_B]			= &t7_pwm_b.hw,
	[CLKID_PWM_C_SEL]		= &t7_pwm_c_sel.hw,
	[CLKID_PWM_C_DIV]		= &t7_pwm_c_div.hw,
	[CLKID_PWM_C]			= &t7_pwm_c.hw,
	[CLKID_PWM_D_SEL]		= &t7_pwm_d_sel.hw,
	[CLKID_PWM_D_DIV]		= &t7_pwm_d_div.hw,
	[CLKID_PWM_D]			= &t7_pwm_d.hw,
	[CLKID_PWM_E_SEL]		= &t7_pwm_e_sel.hw,
	[CLKID_PWM_E_DIV]		= &t7_pwm_e_div.hw,
	[CLKID_PWM_E]			= &t7_pwm_e.hw,
	[CLKID_PWM_F_SEL]		= &t7_pwm_f_sel.hw,
	[CLKID_PWM_F_DIV]		= &t7_pwm_f_div.hw,
	[CLKID_PWM_F]			= &t7_pwm_f.hw,
	[CLKID_PWM_AO_A_SEL]		= &t7_pwm_ao_a_sel.hw,
	[CLKID_PWM_AO_A_DIV]		= &t7_pwm_ao_a_div.hw,
	[CLKID_PWM_AO_A]		= &t7_pwm_ao_a.hw,
	[CLKID_PWM_AO_B_SEL]		= &t7_pwm_ao_b_sel.hw,
	[CLKID_PWM_AO_B_DIV]		= &t7_pwm_ao_b_div.hw,
	[CLKID_PWM_AO_B]		= &t7_pwm_ao_b.hw,
	[CLKID_PWM_AO_C_SEL]		= &t7_pwm_ao_c_sel.hw,
	[CLKID_PWM_AO_C_DIV]		= &t7_pwm_ao_c_div.hw,
	[CLKID_PWM_AO_C]		= &t7_pwm_ao_c.hw,
	[CLKID_PWM_AO_D_SEL]		= &t7_pwm_ao_d_sel.hw,
	[CLKID_PWM_AO_D_DIV]		= &t7_pwm_ao_d_div.hw,
	[CLKID_PWM_AO_D]		= &t7_pwm_ao_d.hw,
	[CLKID_PWM_AO_E_SEL]		= &t7_pwm_ao_e_sel.hw,
	[CLKID_PWM_AO_E_DIV]		= &t7_pwm_ao_e_div.hw,
	[CLKID_PWM_AO_E]		= &t7_pwm_ao_e.hw,
	[CLKID_PWM_AO_F_SEL]		= &t7_pwm_ao_f_sel.hw,
	[CLKID_PWM_AO_F_DIV]		= &t7_pwm_ao_f_div.hw,
	[CLKID_PWM_AO_F]		= &t7_pwm_ao_f.hw,
	[CLKID_PWM_AO_G_SEL]		= &t7_pwm_ao_g_sel.hw,
	[CLKID_PWM_AO_G_DIV]		= &t7_pwm_ao_g_div.hw,
	[CLKID_PWM_AO_G]		= &t7_pwm_ao_g.hw,
	[CLKID_PWM_AO_H_SEL]		= &t7_pwm_ao_h_sel.hw,
	[CLKID_PWM_AO_H_DIV]		= &t7_pwm_ao_h_div.hw,
	[CLKID_PWM_AO_H]		= &t7_pwm_ao_h.hw,
	[CLKID_SYS_DDR]			= &t7_sys_ddr.hw,
	[CLKID_SYS_DOS]			= &t7_sys_dos.hw,
	[CLKID_SYS_MIPI_DSI_A]		= &t7_sys_mipi_dsi_a.hw,
	[CLKID_SYS_MIPI_DSI_B]		= &t7_sys_mipi_dsi_b.hw,
	[CLKID_SYS_ETHPHY]		= &t7_sys_ethphy.hw,
	[CLKID_SYS_MALI]		= &t7_sys_mali.hw,
	[CLKID_SYS_AOCPU]		= &t7_sys_aocpu.hw,
	[CLKID_SYS_AUCPU]		= &t7_sys_aucpu.hw,
	[CLKID_SYS_CEC]			= &t7_sys_cec.hw,
	[CLKID_SYS_GDC]			= &t7_sys_gdc.hw,
	[CLKID_SYS_DESWARP]		= &t7_sys_deswarp.hw,
	[CLKID_SYS_AMPIPE_NAND]		= &t7_sys_ampipe_nand.hw,
	[CLKID_SYS_AMPIPE_ETH]		= &t7_sys_ampipe_eth.hw,
	[CLKID_SYS_AM2AXI0]		= &t7_sys_am2axi0.hw,
	[CLKID_SYS_AM2AXI1]		= &t7_sys_am2axi1.hw,
	[CLKID_SYS_AM2AXI2]		= &t7_sys_am2axi2.hw,
	[CLKID_SYS_SD_EMMC_A]		= &t7_sys_sdemmca.hw,
	[CLKID_SYS_SD_EMMC_B]		= &t7_sys_sdemmcb.hw,
	[CLKID_SYS_SD_EMMC_C]		= &t7_sys_sdemmcc.hw,
	[CLKID_SYS_SMARTCARD]		= &t7_sys_smartcard.hw,
	[CLKID_SYS_ACODEC]		= &t7_sys_acodec.hw,
	[CLKID_SYS_SPIFC]		= &t7_sys_spifc.hw,
	[CLKID_SYS_MSR_CLK]		= &t7_sys_msr_clk.hw,
	[CLKID_SYS_IR_CTRL]		= &t7_sys_ir_ctrl.hw,
	[CLKID_SYS_AUDIO]		= &t7_sys_audio.hw,
	[CLKID_SYS_ETH]			= &t7_sys_eth.hw,
	[CLKID_SYS_UART_A]		= &t7_sys_uart_a.hw,
	[CLKID_SYS_UART_B]		= &t7_sys_uart_b.hw,
	[CLKID_SYS_UART_C]		= &t7_sys_uart_c.hw,
	[CLKID_SYS_UART_D]		= &t7_sys_uart_d.hw,
	[CLKID_SYS_UART_E]		= &t7_sys_uart_e.hw,
	[CLKID_SYS_UART_F]		= &t7_sys_uart_f.hw,
	[CLKID_SYS_AIFIFO]		= &t7_sys_aififo.hw,
	[CLKID_SYS_SPICC2]		= &t7_sys_spicc2.hw,
	[CLKID_SYS_SPICC3]		= &t7_sys_spicc3.hw,
	[CLKID_SYS_SPICC4]		= &t7_sys_spicc4.hw,
	[CLKID_SYS_TS_A73]		= &t7_sys_ts_a73.hw,
	[CLKID_SYS_TS_A53]		= &t7_sys_ts_a53.hw,
	[CLKID_SYS_SPICC5]		= &t7_sys_spicc5.hw,
	[CLKID_SYS_G2D]			= &t7_sys_g2d.hw,
	[CLKID_SYS_SPICC0]		= &t7_sys_spicc0.hw,
	[CLKID_SYS_SPICC1]		= &t7_sys_spicc1.hw,
	[CLKID_SYS_PCIE]		= &t7_sys_pcie.hw,
	[CLKID_SYS_USB]			= &t7_sys_usb.hw,
	[CLKID_SYS_PCIE_PHY]		= &t7_sys_pcie_phy.hw,
	[CLKID_SYS_I2C_AO_A]		= &t7_sys_i2c_ao_a.hw,
	[CLKID_SYS_I2C_AO_B]		= &t7_sys_i2c_ao_b.hw,
	[CLKID_SYS_I2C_M_A]		= &t7_sys_i2c_m_a.hw,
	[CLKID_SYS_I2C_M_B]		= &t7_sys_i2c_m_b.hw,
	[CLKID_SYS_I2C_M_C]		= &t7_sys_i2c_m_c.hw,
	[CLKID_SYS_I2C_M_D]		= &t7_sys_i2c_m_d.hw,
	[CLKID_SYS_I2C_M_E]		= &t7_sys_i2c_m_e.hw,
	[CLKID_SYS_I2C_M_F]		= &t7_sys_i2c_m_f.hw,
	[CLKID_SYS_HDMITX_APB]		= &t7_sys_hdmitx_apb.hw,
	[CLKID_SYS_I2C_S_A]		= &t7_sys_i2c_s_a.hw,
	[CLKID_SYS_HDMIRX_PCLK]		= &t7_sys_hdmirx_pclk.hw,
	[CLKID_SYS_MMC_APB]		= &t7_sys_mmc_apb.hw,
	[CLKID_SYS_MIPI_ISP_PCLK]	= &t7_sys_mipi_isp_pclk.hw,
	[CLKID_SYS_RSA]			= &t7_sys_rsa.hw,
	[CLKID_SYS_PCLK_SYS_APB]	= &t7_sys_pclk_sys_apb.hw,
	[CLKID_SYS_A73PCLK_APB]		= &t7_sys_a73pclk_apb.hw,
	[CLKID_SYS_DSPA]		= &t7_sys_dspa.hw,
	[CLKID_SYS_DSPB]		= &t7_sys_dspb.hw,
	[CLKID_SYS_VPU_INTR]		= &t7_sys_vpu_intr.hw,
	[CLKID_SYS_SAR_ADC]		= &t7_sys_sar_adc.hw,
	[CLKID_SYS_GIC]			= &t7_sys_gic.hw,
	[CLKID_SYS_TS_GPU]		= &t7_sys_ts_gpu.hw,
	[CLKID_SYS_TS_NNA]		= &t7_sys_ts_nna.hw,
	[CLKID_SYS_TS_VPU]		= &t7_sys_ts_vpu.hw,
	[CLKID_SYS_TS_HEVC]		= &t7_sys_ts_hevc.hw,
	[CLKID_SYS_PWM_AB]		= &t7_sys_pwm_ab.hw,
	[CLKID_SYS_PWM_CD]		= &t7_sys_pwm_cd.hw,
	[CLKID_SYS_PWM_EF]		= &t7_sys_pwm_ef.hw,
	[CLKID_SYS_PWM_AO_AB]		= &t7_sys_pwm_ao_ab.hw,
	[CLKID_SYS_PWM_AO_CD]		= &t7_sys_pwm_ao_cd.hw,
	[CLKID_SYS_PWM_AO_EF]		= &t7_sys_pwm_ao_ef.hw,
	[CLKID_SYS_PWM_AO_GH]		= &t7_sys_pwm_ao_gh.hw,
};

/* Convenience table to populate regmap in .probe */
static struct clk_regmap *const t7_periphs_regmaps[] = {
	&t7_rtc_32k_in,
	&t7_rtc_32k_div,
	&t7_rtc_32k_force_sel,
	&t7_rtc_32k_out,
	&t7_rtc_32k_mux0_0,
	&t7_rtc_32k_mux0_1,
	&t7_rtc,
	&t7_cecb_32k_in,
	&t7_cecb_32k_div,
	&t7_cecb_32k_sel_pre,
	&t7_cecb_32k_sel,
	&t7_ceca_32k_in,
	&t7_ceca_32k_div,
	&t7_ceca_32k_sel_pre,
	&t7_ceca_32k_sel,
	&t7_ceca_32k_out,
	&t7_cecb_32k_out,
	&t7_sc_sel,
	&t7_sc_div,
	&t7_sc,
	&t7_dspa_a_sel,
	&t7_dspa_a_div,
	&t7_dspa_a,
	&t7_dspa_b_sel,
	&t7_dspa_b_div,
	&t7_dspa_b,
	&t7_dspa,
	&t7_dspb_a_sel,
	&t7_dspb_a_div,
	&t7_dspb_a,
	&t7_dspb_b_sel,
	&t7_dspb_b_div,
	&t7_dspb_b,
	&t7_dspb,
	&t7_clk_24m,
	&t7_clk_12m,
	&t7_anakin_0_sel,
	&t7_anakin_0_div,
	&t7_anakin_0,
	&t7_anakin_1_sel,
	&t7_anakin_1_div,
	&t7_anakin_1,
	&t7_anakin,
	&t7_anakin_clk,
	&t7_fdiv2_divn_pre,
	&t7_fdiv2_divn,
	&t7_mipi_csi_phy0_sel,
	&t7_mipi_csi_phy0_div,
	&t7_mipi_csi_phy0,
	&t7_mipi_csi_phy1_sel,
	&t7_mipi_csi_phy1_div,
	&t7_mipi_csi_phy1,
	&t7_mipi_csi_phy,
	&t7_mipi_isp_sel,
	&t7_mipi_isp_div,
	&t7_mipi_isp,
	&t7_ts_div,
	&t7_ts,
	&t7_mali_0_sel,
	&t7_mali_0_div,
	&t7_mali_0,
	&t7_mali_1_sel,
	&t7_mali_1_div,
	&t7_mali_1,
	&t7_mali,
	&t7_eth_rmii_sel,
	&t7_eth_rmii_div,
	&t7_eth_rmii,
	&t7_eth_125m,
	&t7_sd_emmc_c_sel,
	&t7_sd_emmc_c_div,
	&t7_sd_emmc_c,
	&t7_sd_emmc_a_sel,
	&t7_sd_emmc_a_div,
	&t7_sd_emmc_a,
	&t7_sd_emmc_b_sel,
	&t7_sd_emmc_b_div,
	&t7_sd_emmc_b,
	&t7_spicc0_sel,
	&t7_spicc0_div,
	&t7_spicc0,
	&t7_spicc1_sel,
	&t7_spicc1_div,
	&t7_spicc1,
	&t7_spicc2_sel,
	&t7_spicc2_div,
	&t7_spicc2,
	&t7_spicc3_sel,
	&t7_spicc3_div,
	&t7_spicc3,
	&t7_spicc4_sel,
	&t7_spicc4_div,
	&t7_spicc4,
	&t7_spicc5_sel,
	&t7_spicc5_div,
	&t7_spicc5,
	&t7_saradc_sel,
	&t7_saradc_div,
	&t7_saradc,
	&t7_pwm_a_sel,
	&t7_pwm_a_div,
	&t7_pwm_a,
	&t7_pwm_b_sel,
	&t7_pwm_b_div,
	&t7_pwm_b,
	&t7_pwm_c_sel,
	&t7_pwm_c_div,
	&t7_pwm_c,
	&t7_pwm_d_sel,
	&t7_pwm_d_div,
	&t7_pwm_d,
	&t7_pwm_e_sel,
	&t7_pwm_e_div,
	&t7_pwm_e,
	&t7_pwm_f_sel,
	&t7_pwm_f_div,
	&t7_pwm_f,
	&t7_pwm_ao_a_sel,
	&t7_pwm_ao_a_div,
	&t7_pwm_ao_a,
	&t7_pwm_ao_b_sel,
	&t7_pwm_ao_b_div,
	&t7_pwm_ao_b,
	&t7_pwm_ao_c_sel,
	&t7_pwm_ao_c_div,
	&t7_pwm_ao_c,
	&t7_pwm_ao_d_sel,
	&t7_pwm_ao_d_div,
	&t7_pwm_ao_d,
	&t7_pwm_ao_e_sel,
	&t7_pwm_ao_e_div,
	&t7_pwm_ao_e,
	&t7_pwm_ao_f_sel,
	&t7_pwm_ao_f_div,
	&t7_pwm_ao_f,
	&t7_pwm_ao_g_sel,
	&t7_pwm_ao_g_div,
	&t7_pwm_ao_g,
	&t7_pwm_ao_h_sel,
	&t7_pwm_ao_h_div,
	&t7_pwm_ao_h,
	&t7_pwm_ao_h,
	&t7_sys_ddr,
	&t7_sys_dos,
	&t7_sys_mipi_dsi_a,
	&t7_sys_mipi_dsi_b,
	&t7_sys_ethphy,
	&t7_sys_mali,
	&t7_sys_aocpu,
	&t7_sys_aucpu,
	&t7_sys_cec,
	&t7_sys_gdc,
	&t7_sys_deswarp,
	&t7_sys_ampipe_nand,
	&t7_sys_ampipe_eth,
	&t7_sys_am2axi0,
	&t7_sys_am2axi1,
	&t7_sys_am2axi2,
	&t7_sys_sdemmca,
	&t7_sys_sdemmcb,
	&t7_sys_sdemmcc,
	&t7_sys_smartcard,
	&t7_sys_acodec,
	&t7_sys_spifc,
	&t7_sys_msr_clk,
	&t7_sys_ir_ctrl,
	&t7_sys_audio,
	&t7_sys_eth,
	&t7_sys_uart_a,
	&t7_sys_uart_b,
	&t7_sys_uart_c,
	&t7_sys_uart_d,
	&t7_sys_uart_e,
	&t7_sys_uart_f,
	&t7_sys_aififo,
	&t7_sys_spicc2,
	&t7_sys_spicc3,
	&t7_sys_spicc4,
	&t7_sys_ts_a73,
	&t7_sys_ts_a53,
	&t7_sys_spicc5,
	&t7_sys_g2d,
	&t7_sys_spicc0,
	&t7_sys_spicc1,
	&t7_sys_pcie,
	&t7_sys_usb,
	&t7_sys_pcie_phy,
	&t7_sys_i2c_ao_a,
	&t7_sys_i2c_ao_b,
	&t7_sys_i2c_m_a,
	&t7_sys_i2c_m_b,
	&t7_sys_i2c_m_c,
	&t7_sys_i2c_m_d,
	&t7_sys_i2c_m_e,
	&t7_sys_i2c_m_f,
	&t7_sys_hdmitx_apb,
	&t7_sys_i2c_s_a,
	&t7_sys_hdmirx_pclk,
	&t7_sys_mmc_apb,
	&t7_sys_mipi_isp_pclk,
	&t7_sys_rsa,
	&t7_sys_pclk_sys_apb,
	&t7_sys_a73pclk_apb,
	&t7_sys_dspa,
	&t7_sys_dspb,
	&t7_sys_vpu_intr,
	&t7_sys_sar_adc,
	&t7_sys_gic,
	&t7_sys_ts_gpu,
	&t7_sys_ts_nna,
	&t7_sys_ts_vpu,
	&t7_sys_ts_hevc,
	&t7_sys_pwm_ab,
	&t7_sys_pwm_cd,
	&t7_sys_pwm_ef,
	&t7_sys_pwm_ao_ab,
	&t7_sys_pwm_ao_cd,
	&t7_sys_pwm_ao_ef,
	&t7_sys_pwm_ao_gh,
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
MODULE_IMPORT_NS("CLK_MESON");
