// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2024, SaluteDevices. All Rights Reserved.
 *
 * Author: Jan Dakinevich <jan.dakinevich@salutedevices.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>

#include "meson-clkc-utils.h"
#include "meson-audio-rstc.h"
#include "clk-regmap.h"
#include "clk-phase.h"
#include "sclk-div.h"
#include "a1-audio.h"

#define AUDIO_PDATA(_name) \
	((const struct clk_parent_data[]) { { .hw = &(_name).hw } })

#define AUDIO_MUX(_name, _reg, _mask, _shift, _pdata)			\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct clk_regmap_mux_data){				\
		.offset = AUDIO_REG_OFFSET(_reg),			\
		.mask = (_mask),					\
		.shift = (_shift),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &clk_regmap_mux_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = ARRAY_SIZE(_pdata),			\
		.flags = CLK_SET_RATE_PARENT,				\
	},								\
}

#define AUDIO_DIV(_name, _reg, _shift, _width, _pdata)			\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct clk_regmap_div_data){				\
		.offset = AUDIO_REG_OFFSET(_reg),			\
		.shift = (_shift),					\
		.width = (_width),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &clk_regmap_divider_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
		.flags = CLK_SET_RATE_PARENT,				\
	},								\
}

#define AUDIO_GATE(_name, _reg, _bit, _pdata)				\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct clk_regmap_gate_data){				\
		.offset = AUDIO_REG_OFFSET(_reg),			\
		.bit_idx = (_bit),					\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &clk_regmap_gate_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
		.flags = CLK_SET_RATE_PARENT,				\
	},								\
}

#define AUDIO_SCLK_DIV(_name, _reg, _div_shift, _div_width,		\
	_hi_shift, _hi_width, _pdata, _set_rate_parent)			\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct meson_sclk_div_data) {				\
		.div = {						\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_div_shift),				\
			.width = (_div_width),				\
		},							\
		.hi = {							\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_hi_shift),				\
			.width = (_hi_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &meson_sclk_div_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
		.flags = (_set_rate_parent) ? CLK_SET_RATE_PARENT : 0,	\
	},								\
}

#define AUDIO_TRIPHASE(_name, _reg, _width, _shift0, _shift1, _shift2,	\
	_pdata)								\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct meson_clk_triphase_data) {			\
		.ph0 = {						\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_shift0),				\
			.width = (_width),				\
		},							\
		.ph1 = {						\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_shift1),				\
			.width = (_width),				\
		},							\
		.ph2 = {						\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_shift2),				\
			.width = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &meson_clk_triphase_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
		.flags = CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT,	\
	},								\
}

#define AUDIO_SCLK_WS(_name, _reg, _width, _shift_ph, _shift_ws,	\
	_pdata)								\
static struct clk_regmap _name = {					\
	.map = AUDIO_REG_MAP(_reg),					\
	.data = &(struct meson_sclk_ws_inv_data) {			\
		.ph = {							\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_shift_ph),				\
			.width = (_width),				\
		},							\
		.ws = {							\
			.reg_off = AUDIO_REG_OFFSET(_reg),		\
			.shift = (_shift_ws),				\
			.width = (_width),				\
		},							\
	},								\
	.hw.init = &(struct clk_init_data) {				\
		.name = #_name,						\
		.ops = &meson_sclk_ws_inv_ops,				\
		.parent_data = (_pdata),				\
		.num_parents = 1,					\
		.flags = CLK_SET_RATE_PARENT | CLK_DUTY_CYCLE_PARENT,	\
	},								\
}

static const struct clk_parent_data a1_pclk_pdata[] = {
	{ .fw_name = "pclk", },
};

AUDIO_GATE(audio_ddr_arb, AUDIO_CLK_GATE_EN0, 0, a1_pclk_pdata);
AUDIO_GATE(audio_tdmin_a, AUDIO_CLK_GATE_EN0, 1, a1_pclk_pdata);
AUDIO_GATE(audio_tdmin_b, AUDIO_CLK_GATE_EN0, 2, a1_pclk_pdata);
AUDIO_GATE(audio_tdmin_lb, AUDIO_CLK_GATE_EN0, 3, a1_pclk_pdata);
AUDIO_GATE(audio_loopback, AUDIO_CLK_GATE_EN0, 4, a1_pclk_pdata);
AUDIO_GATE(audio_tdmout_a, AUDIO_CLK_GATE_EN0, 5, a1_pclk_pdata);
AUDIO_GATE(audio_tdmout_b, AUDIO_CLK_GATE_EN0, 6, a1_pclk_pdata);
AUDIO_GATE(audio_frddr_a, AUDIO_CLK_GATE_EN0, 7, a1_pclk_pdata);
AUDIO_GATE(audio_frddr_b, AUDIO_CLK_GATE_EN0, 8, a1_pclk_pdata);
AUDIO_GATE(audio_toddr_a, AUDIO_CLK_GATE_EN0, 9, a1_pclk_pdata);
AUDIO_GATE(audio_toddr_b, AUDIO_CLK_GATE_EN0, 10, a1_pclk_pdata);
AUDIO_GATE(audio_spdifin, AUDIO_CLK_GATE_EN0, 11, a1_pclk_pdata);
AUDIO_GATE(audio_resample, AUDIO_CLK_GATE_EN0, 12, a1_pclk_pdata);
AUDIO_GATE(audio_eqdrc, AUDIO_CLK_GATE_EN0, 13, a1_pclk_pdata);
AUDIO_GATE(audio_audiolocker, AUDIO_CLK_GATE_EN0, 14, a1_pclk_pdata);

AUDIO_GATE(audio2_ddr_arb, AUDIO2_CLK_GATE_EN0, 0, a1_pclk_pdata);
AUDIO_GATE(audio2_pdm, AUDIO2_CLK_GATE_EN0, 1, a1_pclk_pdata);
AUDIO_GATE(audio2_tdmin_vad, AUDIO2_CLK_GATE_EN0, 2, a1_pclk_pdata);
AUDIO_GATE(audio2_toddr_vad, AUDIO2_CLK_GATE_EN0, 3, a1_pclk_pdata);
AUDIO_GATE(audio2_vad, AUDIO2_CLK_GATE_EN0, 4, a1_pclk_pdata);
AUDIO_GATE(audio2_audiotop, AUDIO2_CLK_GATE_EN0, 7, a1_pclk_pdata);

static const struct clk_parent_data a1_mst_pdata[] = {
	{ .fw_name = "dds_in" },
	{ .fw_name = "fclk_div2" },
	{ .fw_name = "fclk_div3" },
	{ .fw_name = "hifi_pll" },
	{ .fw_name = "xtal" },
};

#define AUDIO_MST_MCLK(_name, _reg)					\
	AUDIO_MUX(_name##_mux, (_reg), 0x7, 24, a1_mst_pdata);		\
	AUDIO_DIV(_name##_div, (_reg), 0, 16,				\
		AUDIO_PDATA(_name##_mux));				\
	AUDIO_GATE(_name, (_reg), 31, AUDIO_PDATA(_name##_div))

AUDIO_MST_MCLK(audio_mst_a_mclk, AUDIO_MCLK_A_CTRL);
AUDIO_MST_MCLK(audio_mst_b_mclk, AUDIO_MCLK_B_CTRL);
AUDIO_MST_MCLK(audio_mst_c_mclk, AUDIO_MCLK_C_CTRL);
AUDIO_MST_MCLK(audio_mst_d_mclk, AUDIO_MCLK_D_CTRL);
AUDIO_MST_MCLK(audio_spdifin_clk, AUDIO_CLK_SPDIFIN_CTRL);
AUDIO_MST_MCLK(audio_eqdrc_clk, AUDIO_CLK_EQDRC_CTRL);

AUDIO_MUX(audio_resample_clk_mux, AUDIO_CLK_RESAMPLE_CTRL, 0xf, 24,
	a1_mst_pdata);
AUDIO_DIV(audio_resample_clk_div, AUDIO_CLK_RESAMPLE_CTRL, 0, 8,
	AUDIO_PDATA(audio_resample_clk_mux));
AUDIO_GATE(audio_resample_clk, AUDIO_CLK_RESAMPLE_CTRL, 31,
	AUDIO_PDATA(audio_resample_clk_div));

AUDIO_MUX(audio_locker_in_clk_mux, AUDIO_CLK_LOCKER_CTRL, 0xf, 8,
	a1_mst_pdata);
AUDIO_DIV(audio_locker_in_clk_div, AUDIO_CLK_LOCKER_CTRL, 0, 8,
	AUDIO_PDATA(audio_locker_in_clk_mux));
AUDIO_GATE(audio_locker_in_clk, AUDIO_CLK_LOCKER_CTRL, 15,
	AUDIO_PDATA(audio_locker_in_clk_div));

AUDIO_MUX(audio_locker_out_clk_mux, AUDIO_CLK_LOCKER_CTRL, 0xf, 24,
	a1_mst_pdata);
AUDIO_DIV(audio_locker_out_clk_div, AUDIO_CLK_LOCKER_CTRL, 16, 8,
	AUDIO_PDATA(audio_locker_out_clk_mux));
AUDIO_GATE(audio_locker_out_clk, AUDIO_CLK_LOCKER_CTRL, 31,
	AUDIO_PDATA(audio_locker_out_clk_div));

AUDIO_MST_MCLK(audio2_vad_mclk, AUDIO2_MCLK_VAD_CTRL);
AUDIO_MST_MCLK(audio2_vad_clk, AUDIO2_CLK_VAD_CTRL);
AUDIO_MST_MCLK(audio2_pdm_dclk, AUDIO2_CLK_PDMIN_CTRL0);
AUDIO_MST_MCLK(audio2_pdm_sysclk, AUDIO2_CLK_PDMIN_CTRL1);

#define AUDIO_MST_SCLK(_name, _reg0, _reg1, _pdata)			\
	AUDIO_GATE(_name##_pre_en, (_reg0), 31, (_pdata));		\
	AUDIO_SCLK_DIV(_name##_div, (_reg0), 20, 10, 0, 0,		\
		AUDIO_PDATA(_name##_pre_en), true);			\
	AUDIO_GATE(_name##_post_en, (_reg0), 30,			\
		AUDIO_PDATA(_name##_div));				\
	AUDIO_TRIPHASE(_name, (_reg1), 1, 0, 2, 4,			\
		AUDIO_PDATA(_name##_post_en))

#define AUDIO_MST_LRCLK(_name, _reg0, _reg1, _pdata)			\
	AUDIO_SCLK_DIV(_name##_div, (_reg0), 0, 10, 10, 10,		\
		(_pdata), false);					\
	AUDIO_TRIPHASE(_name, (_reg1), 1, 1, 3, 5,			\
		AUDIO_PDATA(_name##_div))

AUDIO_MST_SCLK(audio_mst_a_sclk, AUDIO_MST_A_SCLK_CTRL0, AUDIO_MST_A_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_a_mclk));
AUDIO_MST_SCLK(audio_mst_b_sclk, AUDIO_MST_B_SCLK_CTRL0, AUDIO_MST_B_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_b_mclk));
AUDIO_MST_SCLK(audio_mst_c_sclk, AUDIO_MST_C_SCLK_CTRL0, AUDIO_MST_C_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_c_mclk));
AUDIO_MST_SCLK(audio_mst_d_sclk, AUDIO_MST_D_SCLK_CTRL0, AUDIO_MST_D_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_d_mclk));

AUDIO_MST_LRCLK(audio_mst_a_lrclk, AUDIO_MST_A_SCLK_CTRL0, AUDIO_MST_A_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_a_sclk_post_en));
AUDIO_MST_LRCLK(audio_mst_b_lrclk, AUDIO_MST_B_SCLK_CTRL0, AUDIO_MST_B_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_b_sclk_post_en));
AUDIO_MST_LRCLK(audio_mst_c_lrclk, AUDIO_MST_C_SCLK_CTRL0, AUDIO_MST_C_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_c_sclk_post_en));
AUDIO_MST_LRCLK(audio_mst_d_lrclk, AUDIO_MST_D_SCLK_CTRL0, AUDIO_MST_D_SCLK_CTRL1,
	AUDIO_PDATA(audio_mst_d_sclk_post_en));

static const struct clk_parent_data a1_mst_sclk_pdata[] = {
	{ .hw = &audio_mst_a_sclk.hw },
	{ .hw = &audio_mst_b_sclk.hw },
	{ .hw = &audio_mst_c_sclk.hw },
	{ .hw = &audio_mst_d_sclk.hw },
	{ .fw_name = "slv_sclk0" },
	{ .fw_name = "slv_sclk1" },
	{ .fw_name = "slv_sclk2" },
	{ .fw_name = "slv_sclk3" },
	{ .fw_name = "slv_sclk4" },
	{ .fw_name = "slv_sclk5" },
	{ .fw_name = "slv_sclk6" },
	{ .fw_name = "slv_sclk7" },
	{ .fw_name = "slv_sclk8" },
	{ .fw_name = "slv_sclk9" },
};

static const struct clk_parent_data a1_mst_lrclk_pdata[] = {
	{ .hw = &audio_mst_a_lrclk.hw },
	{ .hw = &audio_mst_b_lrclk.hw },
	{ .hw = &audio_mst_c_lrclk.hw },
	{ .hw = &audio_mst_d_lrclk.hw },
	{ .fw_name = "slv_lrclk0" },
	{ .fw_name = "slv_lrclk1" },
	{ .fw_name = "slv_lrclk2" },
	{ .fw_name = "slv_lrclk3" },
	{ .fw_name = "slv_lrclk4" },
	{ .fw_name = "slv_lrclk5" },
	{ .fw_name = "slv_lrclk6" },
	{ .fw_name = "slv_lrclk7" },
	{ .fw_name = "slv_lrclk8" },
	{ .fw_name = "slv_lrclk9" },
};

#define AUDIO_TDM_SCLK(_name, _reg)					\
	AUDIO_MUX(_name##_mux, (_reg), 0xf, 24, a1_mst_sclk_pdata);	\
	AUDIO_GATE(_name##_pre_en, (_reg), 31,				\
		AUDIO_PDATA(_name##_mux));				\
	AUDIO_GATE(_name##_post_en, (_reg), 30,				\
		AUDIO_PDATA(_name##_pre_en));				\
	AUDIO_SCLK_WS(_name, (_reg), 1, 29, 28,				\
		AUDIO_PDATA(_name##_post_en))

#define AUDIO_TDM_LRCLK(_name, _reg)					\
	AUDIO_MUX(_name, (_reg), 0xf, 20, a1_mst_lrclk_pdata)

AUDIO_TDM_SCLK(audio_tdmin_a_sclk, AUDIO_CLK_TDMIN_A_CTRL);
AUDIO_TDM_SCLK(audio_tdmin_b_sclk, AUDIO_CLK_TDMIN_B_CTRL);
AUDIO_TDM_SCLK(audio_tdmin_lb_sclk, AUDIO_CLK_TDMIN_LB_CTRL);
AUDIO_TDM_SCLK(audio_tdmout_a_sclk, AUDIO_CLK_TDMOUT_A_CTRL);
AUDIO_TDM_SCLK(audio_tdmout_b_sclk, AUDIO_CLK_TDMOUT_B_CTRL);

AUDIO_TDM_LRCLK(audio_tdmin_a_lrclk, AUDIO_CLK_TDMIN_A_CTRL);
AUDIO_TDM_LRCLK(audio_tdmin_b_lrclk, AUDIO_CLK_TDMIN_B_CTRL);
AUDIO_TDM_LRCLK(audio_tdmin_lb_lrclk, AUDIO_CLK_TDMIN_LB_CTRL);
AUDIO_TDM_LRCLK(audio_tdmout_a_lrclk, AUDIO_CLK_TDMOUT_A_CTRL);
AUDIO_TDM_LRCLK(audio_tdmout_b_lrclk, AUDIO_CLK_TDMOUT_B_CTRL);

static struct clk_hw *a1_audio_hw_clks[] = {
	[AUD_CLKID_DDR_ARB]		= &audio_ddr_arb.hw,
	[AUD_CLKID_TDMIN_A]		= &audio_tdmin_a.hw,
	[AUD_CLKID_TDMIN_B]		= &audio_tdmin_b.hw,
	[AUD_CLKID_TDMIN_LB]		= &audio_tdmin_lb.hw,
	[AUD_CLKID_LOOPBACK]		= &audio_loopback.hw,
	[AUD_CLKID_TDMOUT_A]		= &audio_tdmout_a.hw,
	[AUD_CLKID_TDMOUT_B]		= &audio_tdmout_b.hw,
	[AUD_CLKID_FRDDR_A]		= &audio_frddr_a.hw,
	[AUD_CLKID_FRDDR_B]		= &audio_frddr_b.hw,
	[AUD_CLKID_TODDR_A]		= &audio_toddr_a.hw,
	[AUD_CLKID_TODDR_B]		= &audio_toddr_b.hw,
	[AUD_CLKID_SPDIFIN]		= &audio_spdifin.hw,
	[AUD_CLKID_RESAMPLE]		= &audio_resample.hw,
	[AUD_CLKID_EQDRC]		= &audio_eqdrc.hw,
	[AUD_CLKID_LOCKER]		= &audio_audiolocker.hw,
	[AUD_CLKID_MST_A_MCLK_SEL]	= &audio_mst_a_mclk_mux.hw,
	[AUD_CLKID_MST_A_MCLK_DIV]	= &audio_mst_a_mclk_div.hw,
	[AUD_CLKID_MST_A_MCLK]		= &audio_mst_a_mclk.hw,
	[AUD_CLKID_MST_B_MCLK_SEL]	= &audio_mst_b_mclk_mux.hw,
	[AUD_CLKID_MST_B_MCLK_DIV]	= &audio_mst_b_mclk_div.hw,
	[AUD_CLKID_MST_B_MCLK]		= &audio_mst_b_mclk.hw,
	[AUD_CLKID_MST_C_MCLK_SEL]	= &audio_mst_c_mclk_mux.hw,
	[AUD_CLKID_MST_C_MCLK_DIV]	= &audio_mst_c_mclk_div.hw,
	[AUD_CLKID_MST_C_MCLK]		= &audio_mst_c_mclk.hw,
	[AUD_CLKID_MST_D_MCLK_SEL]	= &audio_mst_d_mclk_mux.hw,
	[AUD_CLKID_MST_D_MCLK_DIV]	= &audio_mst_d_mclk_div.hw,
	[AUD_CLKID_MST_D_MCLK]		= &audio_mst_d_mclk.hw,
	[AUD_CLKID_RESAMPLE_CLK_SEL]	= &audio_resample_clk_mux.hw,
	[AUD_CLKID_RESAMPLE_CLK_DIV]	= &audio_resample_clk_div.hw,
	[AUD_CLKID_RESAMPLE_CLK]	= &audio_resample_clk.hw,
	[AUD_CLKID_LOCKER_IN_CLK_SEL]	= &audio_locker_in_clk_mux.hw,
	[AUD_CLKID_LOCKER_IN_CLK_DIV]	= &audio_locker_in_clk_div.hw,
	[AUD_CLKID_LOCKER_IN_CLK]	= &audio_locker_in_clk.hw,
	[AUD_CLKID_LOCKER_OUT_CLK_SEL]	= &audio_locker_out_clk_mux.hw,
	[AUD_CLKID_LOCKER_OUT_CLK_DIV]	= &audio_locker_out_clk_div.hw,
	[AUD_CLKID_LOCKER_OUT_CLK]	= &audio_locker_out_clk.hw,
	[AUD_CLKID_SPDIFIN_CLK_SEL]	= &audio_spdifin_clk_mux.hw,
	[AUD_CLKID_SPDIFIN_CLK_DIV]	= &audio_spdifin_clk_div.hw,
	[AUD_CLKID_SPDIFIN_CLK]		= &audio_spdifin_clk.hw,
	[AUD_CLKID_EQDRC_CLK_SEL]	= &audio_eqdrc_clk_mux.hw,
	[AUD_CLKID_EQDRC_CLK_DIV]	= &audio_eqdrc_clk_div.hw,
	[AUD_CLKID_EQDRC_CLK]		= &audio_eqdrc_clk.hw,
	[AUD_CLKID_MST_A_SCLK_PRE_EN]	= &audio_mst_a_sclk_pre_en.hw,
	[AUD_CLKID_MST_A_SCLK_DIV]	= &audio_mst_a_sclk_div.hw,
	[AUD_CLKID_MST_A_SCLK_POST_EN]	= &audio_mst_a_sclk_post_en.hw,
	[AUD_CLKID_MST_A_SCLK]		= &audio_mst_a_sclk.hw,
	[AUD_CLKID_MST_B_SCLK_PRE_EN]	= &audio_mst_b_sclk_pre_en.hw,
	[AUD_CLKID_MST_B_SCLK_DIV]	= &audio_mst_b_sclk_div.hw,
	[AUD_CLKID_MST_B_SCLK_POST_EN]	= &audio_mst_b_sclk_post_en.hw,
	[AUD_CLKID_MST_B_SCLK]		= &audio_mst_b_sclk.hw,
	[AUD_CLKID_MST_C_SCLK_PRE_EN]	= &audio_mst_c_sclk_pre_en.hw,
	[AUD_CLKID_MST_C_SCLK_DIV]	= &audio_mst_c_sclk_div.hw,
	[AUD_CLKID_MST_C_SCLK_POST_EN]	= &audio_mst_c_sclk_post_en.hw,
	[AUD_CLKID_MST_C_SCLK]		= &audio_mst_c_sclk.hw,
	[AUD_CLKID_MST_D_SCLK_PRE_EN]	= &audio_mst_d_sclk_pre_en.hw,
	[AUD_CLKID_MST_D_SCLK_DIV]	= &audio_mst_d_sclk_div.hw,
	[AUD_CLKID_MST_D_SCLK_POST_EN]	= &audio_mst_d_sclk_post_en.hw,
	[AUD_CLKID_MST_D_SCLK]		= &audio_mst_d_sclk.hw,
	[AUD_CLKID_MST_A_LRCLK_DIV]	= &audio_mst_a_lrclk_div.hw,
	[AUD_CLKID_MST_A_LRCLK]		= &audio_mst_a_lrclk.hw,
	[AUD_CLKID_MST_B_LRCLK_DIV]	= &audio_mst_b_lrclk_div.hw,
	[AUD_CLKID_MST_B_LRCLK]		= &audio_mst_b_lrclk.hw,
	[AUD_CLKID_MST_C_LRCLK_DIV]	= &audio_mst_c_lrclk_div.hw,
	[AUD_CLKID_MST_C_LRCLK]		= &audio_mst_c_lrclk.hw,
	[AUD_CLKID_MST_D_LRCLK_DIV]	= &audio_mst_d_lrclk_div.hw,
	[AUD_CLKID_MST_D_LRCLK]		= &audio_mst_d_lrclk.hw,
	[AUD_CLKID_TDMIN_A_SCLK_SEL]	= &audio_tdmin_a_sclk_mux.hw,
	[AUD_CLKID_TDMIN_A_SCLK_PRE_EN]	= &audio_tdmin_a_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_A_SCLK_POST_EN] = &audio_tdmin_a_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_A_SCLK]	= &audio_tdmin_a_sclk.hw,
	[AUD_CLKID_TDMIN_A_LRCLK]	= &audio_tdmin_a_lrclk.hw,
	[AUD_CLKID_TDMIN_B_SCLK_SEL]	= &audio_tdmin_b_sclk_mux.hw,
	[AUD_CLKID_TDMIN_B_SCLK_PRE_EN]	= &audio_tdmin_b_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_B_SCLK_POST_EN] = &audio_tdmin_b_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_B_SCLK]	= &audio_tdmin_b_sclk.hw,
	[AUD_CLKID_TDMIN_B_LRCLK]	= &audio_tdmin_b_lrclk.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_SEL]	= &audio_tdmin_lb_sclk_mux.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_PRE_EN] = &audio_tdmin_lb_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_POST_EN] = &audio_tdmin_lb_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_LB_SCLK]	= &audio_tdmin_lb_sclk.hw,
	[AUD_CLKID_TDMIN_LB_LRCLK]	= &audio_tdmin_lb_lrclk.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_SEL]	= &audio_tdmout_a_sclk_mux.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_PRE_EN] = &audio_tdmout_a_sclk_pre_en.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_POST_EN] = &audio_tdmout_a_sclk_post_en.hw,
	[AUD_CLKID_TDMOUT_A_SCLK]	= &audio_tdmout_a_sclk.hw,
	[AUD_CLKID_TDMOUT_A_LRCLK]	= &audio_tdmout_a_lrclk.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_SEL]	= &audio_tdmout_b_sclk_mux.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_PRE_EN] = &audio_tdmout_b_sclk_pre_en.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_POST_EN] = &audio_tdmout_b_sclk_post_en.hw,
	[AUD_CLKID_TDMOUT_B_SCLK]	= &audio_tdmout_b_sclk.hw,
	[AUD_CLKID_TDMOUT_B_LRCLK]	= &audio_tdmout_b_lrclk.hw,

	[AUD2_CLKID_DDR_ARB]		= &audio2_ddr_arb.hw,
	[AUD2_CLKID_PDM]		= &audio2_pdm.hw,
	[AUD2_CLKID_TDMIN_VAD]		= &audio2_tdmin_vad.hw,
	[AUD2_CLKID_TODDR_VAD]		= &audio2_toddr_vad.hw,
	[AUD2_CLKID_VAD]		= &audio2_vad.hw,
	[AUD2_CLKID_AUDIOTOP]		= &audio2_audiotop.hw,
	[AUD2_CLKID_VAD_MCLK_SEL]	= &audio2_vad_mclk_mux.hw,
	[AUD2_CLKID_VAD_MCLK_DIV]	= &audio2_vad_mclk_div.hw,
	[AUD2_CLKID_VAD_MCLK]		= &audio2_vad_mclk.hw,
	[AUD2_CLKID_VAD_CLK_SEL]	= &audio2_vad_clk_mux.hw,
	[AUD2_CLKID_VAD_CLK_DIV]	= &audio2_vad_clk_div.hw,
	[AUD2_CLKID_VAD_CLK]		= &audio2_vad_clk.hw,
	[AUD2_CLKID_PDM_DCLK_SEL]	= &audio2_pdm_dclk_mux.hw,
	[AUD2_CLKID_PDM_DCLK_DIV]	= &audio2_pdm_dclk_div.hw,
	[AUD2_CLKID_PDM_DCLK]		= &audio2_pdm_dclk.hw,
	[AUD2_CLKID_PDM_SYSCLK_SEL]	= &audio2_pdm_sysclk_mux.hw,
	[AUD2_CLKID_PDM_SYSCLK_DIV]	= &audio2_pdm_sysclk_div.hw,
	[AUD2_CLKID_PDM_SYSCLK]		= &audio2_pdm_sysclk.hw,
};

static struct meson_clk_hw_data a1_audio_clks = {
	.hws = a1_audio_hw_clks,
	.num = ARRAY_SIZE(a1_audio_hw_clks),
};

static struct regmap *a1_audio_map(struct platform_device *pdev,
				   unsigned int index)
{
	char name[32];
	const struct regmap_config cfg = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.name = name,
	};
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return base;

	scnprintf(name, sizeof(name), "%d", index);
	return devm_regmap_init_mmio(&pdev->dev, base, &cfg);
}

static int a1_register_clk(struct platform_device *pdev,
			   struct regmap *map0, struct regmap *map1,
			   struct clk_hw *hw)
{
	struct clk_regmap *clk = container_of(hw, struct clk_regmap, hw);

	if (!hw)
		return 0;

	switch ((unsigned long)clk->map) {
	case AUDIO_RANGE_0:
		clk->map = map0;
		break;
	case AUDIO_RANGE_1:
		clk->map = map1;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	return devm_clk_hw_register(&pdev->dev, hw);
}

static int a1_audio_clkc_probe(struct platform_device *pdev)
{
	struct regmap *map0, *map1;
	struct clk *clk;
	unsigned int i;
	int ret;

	clk = devm_clk_get_enabled(&pdev->dev, "pclk");
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	map0 = a1_audio_map(pdev, 0);
	if (IS_ERR(map0))
		return PTR_ERR(map0);

	map1 = a1_audio_map(pdev, 1);
	if (IS_ERR(map1))
		return PTR_ERR(map1);

	/*
	 * Register and enable AUD2_CLKID_AUDIOTOP clock first. Unless
	 * it is enabled any read/write to 'map0' hangs the CPU.
	 */

	ret = a1_register_clk(pdev, map0, map1,
			      a1_audio_clks.hws[AUD2_CLKID_AUDIOTOP]);
	if (ret)
		return ret;

	ret = clk_prepare_enable(a1_audio_clks.hws[AUD2_CLKID_AUDIOTOP]->clk);
	if (ret)
		return ret;

	for (i = 0; i < a1_audio_clks.num; i++) {
		if (i == AUD2_CLKID_AUDIOTOP)
			continue;

		ret = a1_register_clk(pdev, map0, map1, a1_audio_clks.hws[i]);
		if (ret)
			return ret;
	}

	ret = devm_of_clk_add_hw_provider(&pdev->dev, meson_clk_hw_get,
					  &a1_audio_clks);
	if (ret)
		return ret;

	BUILD_BUG_ON((unsigned long)AUDIO_REG_MAP(AUDIO_SW_RESET0) !=
		     AUDIO_RANGE_0);
	return meson_audio_rstc_register(&pdev->dev, map0,
					 AUDIO_REG_OFFSET(AUDIO_SW_RESET0), 32);
}

static const struct of_device_id a1_audio_clkc_match_table[] = {
	{ .compatible = "amlogic,a1-audio-clkc", },
	{}
};
MODULE_DEVICE_TABLE(of, a1_audio_clkc_match_table);

static struct platform_driver a1_audio_clkc_driver = {
	.probe = a1_audio_clkc_probe,
	.driver = {
		.name = "a1-audio-clkc",
		.of_match_table = a1_audio_clkc_match_table,
	},
};
module_platform_driver(a1_audio_clkc_driver);

MODULE_DESCRIPTION("Amlogic A1 Audio Clock driver");
MODULE_AUTHOR("Jan Dakinevich <jan.dakinevich@salutedevices.com>");
MODULE_LICENSE("GPL");
