// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2024, SaluteDevices. All Rights Reserved.
 *
 * Author: Jan Dakinevich <jan.dakinevich@salutedevices.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <soc/amlogic/reset-meson-aux.h>
#include <dt-bindings/clock/amlogic,a1-audio-clkc.h>

#include "clk-phase.h"
#include "clk-regmap.h"
#include "meson-audio.h"
#include "meson-clkc-utils.h"
#include "sclk-div.h"

#define AUDIO_CLK_GATE_EN0		0x000
#define AUDIO_MCLK_A_CTRL		0x008
#define AUDIO_MCLK_B_CTRL		0x00c
#define AUDIO_MCLK_C_CTRL		0x010
#define AUDIO_MCLK_D_CTRL		0x014
#define AUDIO_SW_RESET0			0x028
#define AUDIO_MST_A_SCLK_CTRL0		0x040
#define AUDIO_MST_A_SCLK_CTRL1		0x044
#define AUDIO_MST_B_SCLK_CTRL0		0x048
#define AUDIO_MST_B_SCLK_CTRL1		0x04c
#define AUDIO_MST_C_SCLK_CTRL0		0x050
#define AUDIO_MST_C_SCLK_CTRL1		0x054
#define AUDIO_MST_D_SCLK_CTRL0		0x058
#define AUDIO_MST_D_SCLK_CTRL1		0x05c
#define AUDIO_CLK_TDMIN_A_CTRL		0x080
#define AUDIO_CLK_TDMIN_B_CTRL		0x084
#define AUDIO_CLK_TDMIN_LB_CTRL		0x08c
#define AUDIO_CLK_TDMOUT_A_CTRL		0x090
#define AUDIO_CLK_TDMOUT_B_CTRL		0x094
#define AUDIO_CLK_SPDIFIN_CTRL		0x09c
#define AUDIO_CLK_RESAMPLEA_CTRL	0x0a4
#define AUDIO_CLK_LOCKER_CTRL		0x0a8
#define AUDIO_CLK_EQDRC_CTRL		0x0c0

#define AUDIO2_CLK81_CTRL		0x000
#define AUDIO2_CLK81_EN			0x004
#define AUDIO2_CLK_GATE_EN0		0x00c
#define AUDIO2_MCLK_VAD_CTRL		0x040
#define AUDIO2_CLK_VAD_CTRL		0x044
#define AUDIO2_MST_VAD_SCLK_CTRL0	0x04c
#define AUDIO2_MST_VAD_SCLK_CTRL1	0x050
#define AUDIO2_CLK_TDMIN_VAD_CTRL	0x054
#define AUDIO2_CLK_PDMIN_CTRL0		0x058
#define AUDIO2_CLK_PDMIN_CTRL1		0x05c

#define AUD_MST_MCLK_MUX(_name, _reg)					\
	AUD_MUX(_name##_sel, (_reg), 0x7, 24, CLK_MUX_ROUND_CLOSEST,	\
		a1_mst_pdata, 0)
#define AUD_MST_MCLK_DIV(_name, _reg)					\
	AUD_DIV(_name##_div, (_reg), 0, 16, CLK_DIVIDER_ROUND_CLOSEST,	\
		aud_##_name##_sel, CLK_SET_RATE_PARENT)
#define AUD_MST_MCLK_GATE(_name, _reg)					\
	AUD_GATE(_name, (_reg), 31,					\
		aud_##_name##_div, CLK_SET_RATE_PARENT)

#define AUD_MST_SCLK_PRE_EN(_name, _reg, _pname)			\
	AUD_GATE(_name##_pre_en, (_reg), 31,				\
		aud_##_pname, 0)
#define AUD_MST_SCLK_DIV(_name, _reg)					\
	AUD_SCLK_DIV(_name##_div, (_reg), 20, 10, 0, 0,			\
		aud_##_name##_pre_en, CLK_SET_RATE_PARENT)
#define AUD_MST_SCLK_POST_EN(_name, _reg)				\
	AUD_GATE(_name##_post_en, (_reg), 30,				\
		aud_##_name##_div, CLK_SET_RATE_PARENT)
#define AUD_MST_SCLK(_name, _reg)					\
	AUD_TRIPHASE(_name, (_reg), 1, 0, 2, 4,				\
		aud_##_name##_post_en, CLK_SET_RATE_PARENT)

#define AUD_MST_LRCLK_DIV(_name, _reg, _pname)				\
	AUD_SCLK_DIV(_name##_div, (_reg), 0, 10, 10, 10,		\
		aud_##_pname, 0)
#define AUD_MST_LRCLK(_name, _reg)					\
	AUD_TRIPHASE(_name, (_reg), 1, 1, 3, 5,				\
		aud_##_name##_div, CLK_SET_RATE_PARENT)

#define AUD_TDM_SCLK_MUX(_name, _reg, _pdata)				\
	AUD_MUX(_name##_sel, (_reg), 0xf, 24, CLK_MUX_ROUND_CLOSEST,	\
		(_pdata), 0)
#define AUD_TDM_SCLK_PRE_EN(_name, _reg)				\
	AUD_GATE(_name##_pre_en, (_reg), 31,				\
		aud_##_name##_sel, CLK_SET_RATE_PARENT)
#define AUD_TDM_SCLK_POST_EN(_name, _reg)				\
	AUD_GATE(_name##_post_en, (_reg), 30,				\
		aud_##_name##_pre_en, CLK_SET_RATE_PARENT)
#define AUD_TDM_SCLK(_name, _reg)					\
	AUD_PHASE(_name, (_reg), 1, 29,					\
		aud_##_name##_post_en,					\
		CLK_DUTY_CYCLE_PARENT | CLK_SET_RATE_PARENT)
#define AUD_TDM_SCLK_WS(_name, _reg)					\
	AUD_SCLK_WS(_name, (_reg), 1, 29, 28,				\
		aud_##_name##_post_en,					\
		CLK_DUTY_CYCLE_PARENT | CLK_SET_RATE_PARENT)

#define AUD_TDM_LRLCK(_name, _reg, _pdata)				\
	AUD_MUX(_name, (_reg), 0xf, 20, CLK_MUX_ROUND_CLOSEST,		\
		(_pdata), CLK_SET_RATE_PARENT)

static const struct clk_parent_data a1_mst_pdata[] = {
	{ .fw_name = "mst_in0" },
	{ .fw_name = "mst_in1" },
	{ .fw_name = "mst_in2" },
	{ .fw_name = "mst_in3" },
	{ .fw_name = "mst_in4" },
};

static const struct clk_parent_data aud_pclk_pdata[] = {
	{ .fw_name = "pclk" },
};

struct clk_regmap aud_ddr_arb =
	AUD_PCLK_GATE(ddr_arb, AUDIO_CLK_GATE_EN0, 0, aud_pclk_pdata);
struct clk_regmap aud_tdmin_a =
	AUD_PCLK_GATE(tdmin_a, AUDIO_CLK_GATE_EN0, 1, aud_pclk_pdata);
struct clk_regmap aud_tdmin_b =
	AUD_PCLK_GATE(tdmin_b, AUDIO_CLK_GATE_EN0, 2, aud_pclk_pdata);
struct clk_regmap aud_tdmin_lb =
	AUD_PCLK_GATE(tdmin_lb, AUDIO_CLK_GATE_EN0, 3, aud_pclk_pdata);
struct clk_regmap aud_loopback =
	AUD_PCLK_GATE(loopback, AUDIO_CLK_GATE_EN0, 4, aud_pclk_pdata);
struct clk_regmap aud_tdmout_a =
	AUD_PCLK_GATE(tdmout_a, AUDIO_CLK_GATE_EN0, 5, aud_pclk_pdata);
struct clk_regmap aud_tdmout_b =
	AUD_PCLK_GATE(tdmout_b, AUDIO_CLK_GATE_EN0, 6, aud_pclk_pdata);
struct clk_regmap aud_frddr_a =
	AUD_PCLK_GATE(frddr_a, AUDIO_CLK_GATE_EN0, 7, aud_pclk_pdata);
struct clk_regmap aud_frddr_b =
	AUD_PCLK_GATE(frddr_b, AUDIO_CLK_GATE_EN0, 8, aud_pclk_pdata);
struct clk_regmap aud_toddr_a =
	AUD_PCLK_GATE(toddr_a, AUDIO_CLK_GATE_EN0, 9, aud_pclk_pdata);
struct clk_regmap aud_toddr_b =
	AUD_PCLK_GATE(toddr_b, AUDIO_CLK_GATE_EN0, 10, aud_pclk_pdata);
struct clk_regmap aud_spdifin =
	AUD_PCLK_GATE(spdifin, AUDIO_CLK_GATE_EN0, 11, aud_pclk_pdata);
struct clk_regmap aud_resample =
	AUD_PCLK_GATE(resample, AUDIO_CLK_GATE_EN0, 12, aud_pclk_pdata);
struct clk_regmap aud_eqdrc =
	AUD_PCLK_GATE(eqdrc, AUDIO_CLK_GATE_EN0, 13, aud_pclk_pdata);
struct clk_regmap aud_audiolocker =
	AUD_PCLK_GATE(audiolocker, AUDIO_CLK_GATE_EN0, 14, aud_pclk_pdata);

struct clk_regmap aud_mst_a_mclk_sel =
	AUD_MST_MCLK_MUX(mst_a_mclk, AUDIO_MCLK_A_CTRL);
struct clk_regmap aud_mst_a_mclk_div =
	AUD_MST_MCLK_DIV(mst_a_mclk, AUDIO_MCLK_A_CTRL);
struct clk_regmap aud_mst_a_mclk =
	AUD_MST_MCLK_GATE(mst_a_mclk, AUDIO_MCLK_A_CTRL);

struct clk_regmap aud_mst_b_mclk_sel =
	AUD_MST_MCLK_MUX(mst_b_mclk, AUDIO_MCLK_B_CTRL);
struct clk_regmap aud_mst_b_mclk_div =
	AUD_MST_MCLK_DIV(mst_b_mclk, AUDIO_MCLK_B_CTRL);
struct clk_regmap aud_mst_b_mclk =
	AUD_MST_MCLK_GATE(mst_b_mclk, AUDIO_MCLK_B_CTRL);

struct clk_regmap aud_mst_c_mclk_sel =
	AUD_MST_MCLK_MUX(mst_c_mclk, AUDIO_MCLK_C_CTRL);
struct clk_regmap aud_mst_c_mclk_div =
	AUD_MST_MCLK_DIV(mst_c_mclk, AUDIO_MCLK_C_CTRL);
struct clk_regmap aud_mst_c_mclk =
	AUD_MST_MCLK_GATE(mst_c_mclk, AUDIO_MCLK_C_CTRL);

struct clk_regmap aud_mst_d_mclk_sel =
	AUD_MST_MCLK_MUX(mst_d_mclk, AUDIO_MCLK_D_CTRL);
struct clk_regmap aud_mst_d_mclk_div =
	AUD_MST_MCLK_DIV(mst_d_mclk, AUDIO_MCLK_D_CTRL);
struct clk_regmap aud_mst_d_mclk =
	AUD_MST_MCLK_GATE(mst_d_mclk, AUDIO_MCLK_D_CTRL);

struct clk_regmap aud_mst_a_sclk_pre_en =
	AUD_MST_SCLK_PRE_EN(mst_a_sclk, AUDIO_MST_A_SCLK_CTRL0, mst_a_mclk);
struct clk_regmap aud_mst_a_sclk_div =
	AUD_MST_SCLK_DIV(mst_a_sclk, AUDIO_MST_A_SCLK_CTRL0);
struct clk_regmap aud_mst_a_sclk_post_en =
	AUD_MST_SCLK_POST_EN(mst_a_sclk, AUDIO_MST_A_SCLK_CTRL0);
struct clk_regmap aud_mst_a_sclk =
	AUD_MST_SCLK(mst_a_sclk, AUDIO_MST_A_SCLK_CTRL1);

struct clk_regmap aud_mst_b_sclk_pre_en =
	AUD_MST_SCLK_PRE_EN(mst_b_sclk, AUDIO_MST_B_SCLK_CTRL0, mst_b_mclk);
struct clk_regmap aud_mst_b_sclk_div =
	AUD_MST_SCLK_DIV(mst_b_sclk, AUDIO_MST_B_SCLK_CTRL0);
struct clk_regmap aud_mst_b_sclk_post_en =
	AUD_MST_SCLK_POST_EN(mst_b_sclk, AUDIO_MST_B_SCLK_CTRL0);
struct clk_regmap aud_mst_b_sclk =
	AUD_MST_SCLK(mst_b_sclk, AUDIO_MST_B_SCLK_CTRL1);

struct clk_regmap aud_mst_c_sclk_pre_en =
	AUD_MST_SCLK_PRE_EN(mst_c_sclk, AUDIO_MST_C_SCLK_CTRL0, mst_c_mclk);
struct clk_regmap aud_mst_c_sclk_div =
	AUD_MST_SCLK_DIV(mst_c_sclk, AUDIO_MST_C_SCLK_CTRL0);
struct clk_regmap aud_mst_c_sclk_post_en =
	AUD_MST_SCLK_POST_EN(mst_c_sclk, AUDIO_MST_C_SCLK_CTRL0);
struct clk_regmap aud_mst_c_sclk =
	AUD_MST_SCLK(mst_c_sclk, AUDIO_MST_C_SCLK_CTRL1);

struct clk_regmap aud_mst_d_sclk_pre_en =
	AUD_MST_SCLK_PRE_EN(mst_d_sclk, AUDIO_MST_D_SCLK_CTRL0, mst_d_mclk);
struct clk_regmap aud_mst_d_sclk_div =
	AUD_MST_SCLK_DIV(mst_d_sclk, AUDIO_MST_D_SCLK_CTRL0);
struct clk_regmap aud_mst_d_sclk_post_en =
	AUD_MST_SCLK_POST_EN(mst_d_sclk, AUDIO_MST_D_SCLK_CTRL0);
struct clk_regmap aud_mst_d_sclk =
	AUD_MST_SCLK(mst_d_sclk, AUDIO_MST_D_SCLK_CTRL1);

struct clk_regmap aud_mst_a_lrclk_div =
	AUD_MST_LRCLK_DIV(mst_a_lrclk, AUDIO_MST_A_SCLK_CTRL0,
			  mst_a_sclk_post_en);
struct clk_regmap aud_mst_a_lrclk =
	AUD_MST_LRCLK(mst_a_lrclk, AUDIO_MST_A_SCLK_CTRL1);

struct clk_regmap aud_mst_b_lrclk_div =
	AUD_MST_LRCLK_DIV(mst_b_lrclk, AUDIO_MST_B_SCLK_CTRL0,
			  mst_b_sclk_post_en);
struct clk_regmap aud_mst_b_lrclk =
	AUD_MST_LRCLK(mst_b_lrclk, AUDIO_MST_B_SCLK_CTRL1);

struct clk_regmap aud_mst_c_lrclk_div =
	AUD_MST_LRCLK_DIV(mst_c_lrclk, AUDIO_MST_C_SCLK_CTRL0,
			  mst_c_sclk_post_en);
struct clk_regmap aud_mst_c_lrclk =
	AUD_MST_LRCLK(mst_c_lrclk, AUDIO_MST_C_SCLK_CTRL1);

struct clk_regmap aud_mst_d_lrclk_div =
	AUD_MST_LRCLK_DIV(mst_d_lrclk, AUDIO_MST_D_SCLK_CTRL0,
			  mst_d_sclk_post_en);
struct clk_regmap aud_mst_d_lrclk =
	AUD_MST_LRCLK(mst_d_lrclk, AUDIO_MST_D_SCLK_CTRL1);

static const struct clk_parent_data aud_mst_sclk_pdata[] = {
	{ .hw = &aud_mst_a_sclk.hw, .index = -1 },
	{ .hw = &aud_mst_b_sclk.hw, .index = -1 },
	{ .hw = &aud_mst_c_sclk.hw, .index = -1 },
	{ .hw = &aud_mst_d_sclk.hw, .index = -1 },
	{ },
	{ },
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

static const struct clk_parent_data aud_mst_lrclk_pdata[] = {
	{ .hw = &aud_mst_a_lrclk.hw, .index = -1 },
	{ .hw = &aud_mst_b_lrclk.hw, .index = -1 },
	{ .hw = &aud_mst_c_lrclk.hw, .index = -1 },
	{ .hw = &aud_mst_d_lrclk.hw, .index = -1 },
	{ },
	{ },
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

struct clk_regmap aud_tdmin_a_sclk_sel =
	AUD_TDM_SCLK_MUX(tdmin_a_sclk, AUDIO_CLK_TDMIN_A_CTRL,
			 aud_mst_sclk_pdata);
struct clk_regmap aud_tdmin_a_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmin_a_sclk, AUDIO_CLK_TDMIN_A_CTRL);
struct clk_regmap aud_tdmin_a_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmin_a_sclk, AUDIO_CLK_TDMIN_A_CTRL);
struct clk_regmap aud_tdmin_a_sclk =
	AUD_TDM_SCLK(tdmin_a_sclk, AUDIO_CLK_TDMIN_A_CTRL);
struct clk_regmap aud_tdmin_a_lrclk =
	AUD_TDM_LRLCK(tdmin_a_lrclk, AUDIO_CLK_TDMIN_A_CTRL,
		      aud_mst_lrclk_pdata);

struct clk_regmap aud_tdmin_b_sclk_sel =
	AUD_TDM_SCLK_MUX(tdmin_b_sclk, AUDIO_CLK_TDMIN_B_CTRL,
			 aud_mst_sclk_pdata);
struct clk_regmap aud_tdmin_b_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmin_b_sclk, AUDIO_CLK_TDMIN_B_CTRL);
struct clk_regmap aud_tdmin_b_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmin_b_sclk, AUDIO_CLK_TDMIN_B_CTRL);
struct clk_regmap aud_tdmin_b_sclk =
	AUD_TDM_SCLK(tdmin_b_sclk, AUDIO_CLK_TDMIN_B_CTRL);
struct clk_regmap aud_tdmin_b_lrclk =
	AUD_TDM_LRLCK(tdmin_b_lrclk, AUDIO_CLK_TDMIN_B_CTRL,
		      aud_mst_lrclk_pdata);

struct clk_regmap aud_tdmin_lb_sclk_sel =
	AUD_TDM_SCLK_MUX(tdmin_lb_sclk, AUDIO_CLK_TDMIN_LB_CTRL,
			 aud_mst_sclk_pdata);
struct clk_regmap aud_tdmin_lb_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmin_lb_sclk, AUDIO_CLK_TDMIN_LB_CTRL);
struct clk_regmap aud_tdmin_lb_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmin_lb_sclk, AUDIO_CLK_TDMIN_LB_CTRL);
struct clk_regmap aud_tdmin_lb_sclk =
	AUD_TDM_SCLK(tdmin_lb_sclk, AUDIO_CLK_TDMIN_LB_CTRL);
struct clk_regmap aud_tdmin_lb_lrclk =
	AUD_TDM_LRLCK(tdmin_lb_lrclk, AUDIO_CLK_TDMIN_LB_CTRL,
		      aud_mst_lrclk_pdata);

struct clk_regmap aud_tdmout_a_sclk_sel =
	AUD_TDM_SCLK_MUX(tdmout_a_sclk, AUDIO_CLK_TDMOUT_A_CTRL,
			 aud_mst_sclk_pdata);
struct clk_regmap aud_tdmout_a_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmout_a_sclk, AUDIO_CLK_TDMOUT_A_CTRL);
struct clk_regmap aud_tdmout_a_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmout_a_sclk, AUDIO_CLK_TDMOUT_A_CTRL);
struct clk_regmap aud_tdmout_a_sclk =
	AUD_TDM_SCLK_WS(tdmout_a_sclk, AUDIO_CLK_TDMOUT_A_CTRL);
struct clk_regmap aud_tdmout_a_lrclk =
	AUD_TDM_LRLCK(tdmout_a_lrclk, AUDIO_CLK_TDMOUT_A_CTRL,
		      aud_mst_lrclk_pdata);

struct clk_regmap aud_tdmout_b_sclk_sel =
	AUD_TDM_SCLK_MUX(tdmout_b_sclk, AUDIO_CLK_TDMOUT_B_CTRL,
			 aud_mst_sclk_pdata);
struct clk_regmap aud_tdmout_b_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmout_b_sclk, AUDIO_CLK_TDMOUT_B_CTRL);
struct clk_regmap aud_tdmout_b_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmout_b_sclk, AUDIO_CLK_TDMOUT_B_CTRL);
struct clk_regmap aud_tdmout_b_sclk =
	AUD_TDM_SCLK_WS(tdmout_b_sclk, AUDIO_CLK_TDMOUT_B_CTRL);
struct clk_regmap aud_tdmout_b_lrclk =
	AUD_TDM_LRLCK(tdmout_b_lrclk, AUDIO_CLK_TDMOUT_B_CTRL,
		      aud_mst_lrclk_pdata);

struct clk_regmap aud_spdifin_clk_sel =
	AUD_MST_MCLK_MUX(spdifin_clk, AUDIO_CLK_SPDIFIN_CTRL);
struct clk_regmap aud_spdifin_clk_div =
	AUD_MST_MCLK_DIV(spdifin_clk, AUDIO_CLK_SPDIFIN_CTRL);
struct clk_regmap aud_spdifin_clk =
	AUD_MST_MCLK_GATE(spdifin_clk, AUDIO_CLK_SPDIFIN_CTRL);

struct clk_regmap aud_resample_clk_sel =
	AUD_MUX(resample_clk_sel, AUDIO_CLK_RESAMPLEA_CTRL, 0xf, 24,
		CLK_MUX_ROUND_CLOSEST, a1_mst_pdata, 0);
struct clk_regmap aud_resample_clk_div =
	AUD_DIV(resample_clk_div, AUDIO_CLK_RESAMPLEA_CTRL, 0, 8,
		CLK_DIVIDER_ROUND_CLOSEST, aud_resample_clk_sel,
		CLK_SET_RATE_PARENT);
struct clk_regmap aud_resample_clk =
	AUD_GATE(resample_clk, AUDIO_CLK_RESAMPLEA_CTRL, 31,
		 aud_resample_clk_div, CLK_SET_RATE_PARENT);

struct clk_regmap aud_eqdrc_clk_sel =
	AUD_MST_MCLK_MUX(eqdrc_clk, AUDIO_CLK_EQDRC_CTRL);
struct clk_regmap aud_eqdrc_clk_div =
	AUD_MST_MCLK_DIV(eqdrc_clk, AUDIO_CLK_EQDRC_CTRL);
struct clk_regmap aud_eqdrc_clk =
	AUD_MST_MCLK_GATE(eqdrc_clk, AUDIO_CLK_EQDRC_CTRL);

struct clk_regmap aud_locker_in_clk_sel =
	AUD_MUX(locker_in_clk_sel, AUDIO_CLK_LOCKER_CTRL, 0xf, 8,
		CLK_MUX_ROUND_CLOSEST, a1_mst_pdata, 0);
struct clk_regmap aud_locker_in_clk_div =
	AUD_DIV(locker_in_clk_div, AUDIO_CLK_LOCKER_CTRL, 0, 8,
		CLK_DIVIDER_ROUND_CLOSEST, aud_locker_in_clk_sel,
		CLK_SET_RATE_PARENT);
struct clk_regmap aud_locker_in_clk =
	AUD_GATE(locker_in_clk, AUDIO_CLK_LOCKER_CTRL, 15,
		 aud_locker_in_clk_div, CLK_SET_RATE_PARENT);

struct clk_regmap aud_locker_out_clk_sel =
	AUD_MUX(locker_out_clk_sel, AUDIO_CLK_LOCKER_CTRL, 0xf, 24,
		CLK_MUX_ROUND_CLOSEST, a1_mst_pdata, 0);
struct clk_regmap aud_locker_out_clk_div =
	AUD_DIV(locker_out_clk_div, AUDIO_CLK_LOCKER_CTRL, 16, 8,
		CLK_DIVIDER_ROUND_CLOSEST, aud_locker_out_clk_sel,
		CLK_SET_RATE_PARENT);
struct clk_regmap aud_locker_out_clk =
	AUD_GATE(locker_out_clk, AUDIO_CLK_LOCKER_CTRL, 31,
		 aud_locker_out_clk_div, CLK_SET_RATE_PARENT);

static struct clk_hw *a1_audio_clkc_hws[] = {
	[AUD_CLKID_DDR_ARB]		= &aud_ddr_arb.hw,
	[AUD_CLKID_TDMIN_A]		= &aud_tdmin_a.hw,
	[AUD_CLKID_TDMIN_B]		= &aud_tdmin_b.hw,
	[AUD_CLKID_TDMIN_LB]		= &aud_tdmin_lb.hw,
	[AUD_CLKID_LOOPBACK]		= &aud_loopback.hw,
	[AUD_CLKID_TDMOUT_A]		= &aud_tdmout_a.hw,
	[AUD_CLKID_TDMOUT_B]		= &aud_tdmout_b.hw,
	[AUD_CLKID_FRDDR_A]		= &aud_frddr_a.hw,
	[AUD_CLKID_FRDDR_B]		= &aud_frddr_b.hw,
	[AUD_CLKID_TODDR_A]		= &aud_toddr_a.hw,
	[AUD_CLKID_TODDR_B]		= &aud_toddr_b.hw,
	[AUD_CLKID_SPDIFIN]		= &aud_spdifin.hw,
	[AUD_CLKID_RESAMPLE]		= &aud_resample.hw,
	[AUD_CLKID_EQDRC]		= &aud_eqdrc.hw,
	[AUD_CLKID_LOCKER]		= &aud_audiolocker.hw,
	[AUD_CLKID_MST_A_MCLK_SEL]	= &aud_mst_a_mclk_sel.hw,
	[AUD_CLKID_MST_A_MCLK_DIV]	= &aud_mst_a_mclk_div.hw,
	[AUD_CLKID_MST_A_MCLK]		= &aud_mst_a_mclk.hw,
	[AUD_CLKID_MST_B_MCLK_SEL]	= &aud_mst_b_mclk_sel.hw,
	[AUD_CLKID_MST_B_MCLK_DIV]	= &aud_mst_b_mclk_div.hw,
	[AUD_CLKID_MST_B_MCLK]		= &aud_mst_b_mclk.hw,
	[AUD_CLKID_MST_C_MCLK_SEL]	= &aud_mst_c_mclk_sel.hw,
	[AUD_CLKID_MST_C_MCLK_DIV]	= &aud_mst_c_mclk_div.hw,
	[AUD_CLKID_MST_C_MCLK]		= &aud_mst_c_mclk.hw,
	[AUD_CLKID_MST_D_MCLK_SEL]	= &aud_mst_d_mclk_sel.hw,
	[AUD_CLKID_MST_D_MCLK_DIV]	= &aud_mst_d_mclk_div.hw,
	[AUD_CLKID_MST_D_MCLK]		= &aud_mst_d_mclk.hw,
	[AUD_CLKID_MST_A_SCLK_PRE_EN]	= &aud_mst_a_sclk_pre_en.hw,
	[AUD_CLKID_MST_A_SCLK_DIV]	= &aud_mst_a_sclk_div.hw,
	[AUD_CLKID_MST_A_SCLK_POST_EN]	= &aud_mst_a_sclk_post_en.hw,
	[AUD_CLKID_MST_A_SCLK]		= &aud_mst_a_sclk.hw,
	[AUD_CLKID_MST_B_SCLK_PRE_EN]	= &aud_mst_b_sclk_pre_en.hw,
	[AUD_CLKID_MST_B_SCLK_DIV]	= &aud_mst_b_sclk_div.hw,
	[AUD_CLKID_MST_B_SCLK_POST_EN]	= &aud_mst_b_sclk_post_en.hw,
	[AUD_CLKID_MST_B_SCLK]		= &aud_mst_b_sclk.hw,
	[AUD_CLKID_MST_C_SCLK_PRE_EN]	= &aud_mst_c_sclk_pre_en.hw,
	[AUD_CLKID_MST_C_SCLK_DIV]	= &aud_mst_c_sclk_div.hw,
	[AUD_CLKID_MST_C_SCLK_POST_EN]	= &aud_mst_c_sclk_post_en.hw,
	[AUD_CLKID_MST_C_SCLK]		= &aud_mst_c_sclk.hw,
	[AUD_CLKID_MST_D_SCLK_PRE_EN]	= &aud_mst_d_sclk_pre_en.hw,
	[AUD_CLKID_MST_D_SCLK_DIV]	= &aud_mst_d_sclk_div.hw,
	[AUD_CLKID_MST_D_SCLK_POST_EN]	= &aud_mst_d_sclk_post_en.hw,
	[AUD_CLKID_MST_D_SCLK]		= &aud_mst_d_sclk.hw,
	[AUD_CLKID_MST_A_LRCLK_DIV]	= &aud_mst_a_lrclk_div.hw,
	[AUD_CLKID_MST_A_LRCLK]		= &aud_mst_a_lrclk.hw,
	[AUD_CLKID_MST_B_LRCLK_DIV]	= &aud_mst_b_lrclk_div.hw,
	[AUD_CLKID_MST_B_LRCLK]		= &aud_mst_b_lrclk.hw,
	[AUD_CLKID_MST_C_LRCLK_DIV]	= &aud_mst_c_lrclk_div.hw,
	[AUD_CLKID_MST_C_LRCLK]		= &aud_mst_c_lrclk.hw,
	[AUD_CLKID_MST_D_LRCLK_DIV]	= &aud_mst_d_lrclk_div.hw,
	[AUD_CLKID_MST_D_LRCLK]		= &aud_mst_d_lrclk.hw,
	[AUD_CLKID_TDMIN_A_SCLK_SEL]	= &aud_tdmin_a_sclk_sel.hw,
	[AUD_CLKID_TDMIN_A_SCLK_PRE_EN]	= &aud_tdmin_a_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_A_SCLK_POST_EN] = &aud_tdmin_a_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_A_SCLK]	= &aud_tdmin_a_sclk.hw,
	[AUD_CLKID_TDMIN_A_LRCLK]	= &aud_tdmin_a_lrclk.hw,
	[AUD_CLKID_TDMIN_B_SCLK_SEL]	= &aud_tdmin_b_sclk_sel.hw,
	[AUD_CLKID_TDMIN_B_SCLK_PRE_EN]	= &aud_tdmin_b_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_B_SCLK_POST_EN] = &aud_tdmin_b_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_B_SCLK]	= &aud_tdmin_b_sclk.hw,
	[AUD_CLKID_TDMIN_B_LRCLK]	= &aud_tdmin_b_lrclk.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_SEL]	= &aud_tdmin_lb_sclk_sel.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_PRE_EN] = &aud_tdmin_lb_sclk_pre_en.hw,
	[AUD_CLKID_TDMIN_LB_SCLK_POST_EN] = &aud_tdmin_lb_sclk_post_en.hw,
	[AUD_CLKID_TDMIN_LB_SCLK]	= &aud_tdmin_lb_sclk.hw,
	[AUD_CLKID_TDMIN_LB_LRCLK]	= &aud_tdmin_lb_lrclk.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_SEL]	= &aud_tdmout_a_sclk_sel.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_PRE_EN] = &aud_tdmout_a_sclk_pre_en.hw,
	[AUD_CLKID_TDMOUT_A_SCLK_POST_EN] = &aud_tdmout_a_sclk_post_en.hw,
	[AUD_CLKID_TDMOUT_A_SCLK]	= &aud_tdmout_a_sclk.hw,
	[AUD_CLKID_TDMOUT_A_LRCLK]	= &aud_tdmout_a_lrclk.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_SEL]	= &aud_tdmout_b_sclk_sel.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_PRE_EN] = &aud_tdmout_b_sclk_pre_en.hw,
	[AUD_CLKID_TDMOUT_B_SCLK_POST_EN] = &aud_tdmout_b_sclk_post_en.hw,
	[AUD_CLKID_TDMOUT_B_SCLK]	= &aud_tdmout_b_sclk.hw,
	[AUD_CLKID_TDMOUT_B_LRCLK]	= &aud_tdmout_b_lrclk.hw,
	[AUD_CLKID_SPDIFIN_CLK_SEL]	= &aud_spdifin_clk_sel.hw,
	[AUD_CLKID_SPDIFIN_CLK_DIV]	= &aud_spdifin_clk_div.hw,
	[AUD_CLKID_SPDIFIN_CLK]		= &aud_spdifin_clk.hw,
	[AUD_CLKID_RESAMPLE_CLK_SEL]	= &aud_resample_clk_sel.hw,
	[AUD_CLKID_RESAMPLE_CLK_DIV]	= &aud_resample_clk_div.hw,
	[AUD_CLKID_RESAMPLE_CLK]	= &aud_resample_clk.hw,
	[AUD_CLKID_EQDRC_CLK_SEL]	= &aud_eqdrc_clk_sel.hw,
	[AUD_CLKID_EQDRC_CLK_DIV]	= &aud_eqdrc_clk_div.hw,
	[AUD_CLKID_EQDRC_CLK]		= &aud_eqdrc_clk.hw,
	[AUD_CLKID_LOCKER_IN_CLK_SEL]	= &aud_locker_in_clk_sel.hw,
	[AUD_CLKID_LOCKER_IN_CLK_DIV]	= &aud_locker_in_clk_div.hw,
	[AUD_CLKID_LOCKER_IN_CLK]	= &aud_locker_in_clk.hw,
	[AUD_CLKID_LOCKER_OUT_CLK_SEL]	= &aud_locker_out_clk_sel.hw,
	[AUD_CLKID_LOCKER_OUT_CLK_DIV]	= &aud_locker_out_clk_div.hw,
	[AUD_CLKID_LOCKER_OUT_CLK]	= &aud_locker_out_clk.hw,
};

static struct clk_regmap aud_vad_clk81 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = AUDIO2_CLK81_EN,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "aud_vad_clk81",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "pclk",
		},
		.num_parents = 1,
	},
};

static struct clk_regmap aud_vad_sysclk_a_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = AUDIO2_CLK81_CTRL,
		.shift = 0,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "aud_vad_sysclk_a_div",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.hw = &aud_vad_clk81.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap aud_vad_sysclk_a = {
	.data = &(struct clk_regmap_gate_data){
		.offset = AUDIO2_CLK81_CTRL,
		.bit_idx = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "aud_vad_sysclk_a",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.hw = &aud_vad_sysclk_a_div.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap aud_vad_sysclk_b_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = AUDIO2_CLK81_CTRL,
		.shift = 16,
		.width = 8,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "aud_vad_sysclk_b_div",
		.ops = &clk_regmap_divider_ops,
		.parent_data = &(const struct clk_parent_data) {
			.hw = &aud_vad_clk81.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap aud_vad_sysclk_b = {
	.data = &(struct clk_regmap_gate_data){
		.offset = AUDIO2_CLK81_CTRL,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "aud_vad_sysclk_b",
		.ops = &clk_regmap_gate_ops,
		.parent_data = &(const struct clk_parent_data) {
			.hw = &aud_vad_sysclk_b_div.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static const struct clk_parent_data aud_vad_sysclk_pdata[] = {
	{ .hw = &aud_vad_sysclk_a.hw },
	{ .hw = &aud_vad_sysclk_b.hw },
};

static struct clk_regmap aud_vad_sysclk = {
	.data = &(struct clk_regmap_mux_data){
		.offset = AUDIO2_CLK81_CTRL,
		.mask = 0x1,
		.shift = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "aud_vad_sysclk",
		.ops = &clk_regmap_mux_ops,
		.parent_data = aud_vad_sysclk_pdata,
		.num_parents = ARRAY_SIZE(aud_vad_sysclk_pdata),
		.flags = CLK_SET_RATE_NO_REPARENT,
	},
};

static const struct clk_parent_data aud_vad_pclk_pdata[] = {
	{ .hw = &aud_vad_sysclk.hw },
};

struct clk_regmap aud_vad_ddr_arb =
	AUD_PCLK_GATE(vad_ddr_arb, AUDIO2_CLK_GATE_EN0, 0, aud_vad_pclk_pdata);
struct clk_regmap aud_vad_pdm =
	AUD_PCLK_GATE(vad_pdm, AUDIO2_CLK_GATE_EN0, 1, aud_vad_pclk_pdata);
struct clk_regmap aud_vad_tdmin_vad =
	AUD_PCLK_GATE(vad_tdmin_vad, AUDIO2_CLK_GATE_EN0, 2, aud_vad_pclk_pdata);
struct clk_regmap aud_vad_toddr_vad =
	AUD_PCLK_GATE(vad_toddr_vad, AUDIO2_CLK_GATE_EN0, 3, aud_vad_pclk_pdata);
struct clk_regmap aud_vad_tovad =
	AUD_PCLK_GATE(tovad, AUDIO2_CLK_GATE_EN0, 4, aud_vad_pclk_pdata);
struct clk_regmap aud_vad_toaudiotop =
	AUD_PCLK_GATE(vad_toaudiotop, AUDIO2_CLK_GATE_EN0, 7, aud_vad_pclk_pdata);

struct clk_regmap aud_vad_mst_vad_mclk_sel =
	AUD_MST_MCLK_MUX(vad_mst_vad_mclk, AUDIO2_MCLK_VAD_CTRL);
struct clk_regmap aud_vad_mst_vad_mclk_div =
	AUD_MST_MCLK_DIV(vad_mst_vad_mclk, AUDIO2_MCLK_VAD_CTRL);
struct clk_regmap aud_vad_mst_vad_mclk =
	AUD_MST_MCLK_GATE(vad_mst_vad_mclk, AUDIO2_MCLK_VAD_CTRL);

struct clk_regmap aud_vad_mst_vad_sclk_pre_en =
	AUD_MST_SCLK_PRE_EN(vad_mst_vad_sclk, AUDIO2_MST_VAD_SCLK_CTRL0,
			    vad_mst_vad_mclk);
struct clk_regmap aud_vad_mst_vad_sclk_div =
	AUD_MST_SCLK_DIV(vad_mst_vad_sclk, AUDIO2_MST_VAD_SCLK_CTRL0);
struct clk_regmap aud_vad_mst_vad_sclk_post_en =
	AUD_MST_SCLK_POST_EN(vad_mst_vad_sclk, AUDIO2_MST_VAD_SCLK_CTRL0);
struct clk_regmap aud_vad_mst_vad_sclk =
	AUD_MST_SCLK(vad_mst_vad_sclk, AUDIO2_MST_VAD_SCLK_CTRL1);

struct clk_regmap aud_vad_mst_vad_lrclk_div =
	AUD_MST_LRCLK_DIV(vad_mst_vad_lrclk, AUDIO2_MST_VAD_SCLK_CTRL0,
			  vad_mst_vad_sclk_post_en);
struct clk_regmap aud_vad_mst_vad_lrclk =
	AUD_MST_LRCLK(vad_mst_vad_lrclk, AUDIO2_MST_VAD_SCLK_CTRL1);

struct clk_regmap aud_vad_pdm_dclk_sel =
	AUD_MST_MCLK_MUX(vad_pdm_dclk, AUDIO2_CLK_PDMIN_CTRL0);
struct clk_regmap aud_vad_pdm_dclk_div =
	AUD_MST_MCLK_DIV(vad_pdm_dclk, AUDIO2_CLK_PDMIN_CTRL0);
struct clk_regmap aud_vad_pdm_dclk =
	AUD_MST_MCLK_GATE(vad_pdm_dclk, AUDIO2_CLK_PDMIN_CTRL0);

struct clk_regmap aud_vad_pdm_sysclk_sel =
	AUD_MST_MCLK_MUX(vad_pdm_sysclk, AUDIO2_CLK_PDMIN_CTRL1);
struct clk_regmap aud_vad_pdm_sysclk_div =
	AUD_MST_MCLK_DIV(vad_pdm_sysclk, AUDIO2_CLK_PDMIN_CTRL1);
struct clk_regmap aud_vad_pdm_sysclk =
	AUD_MST_MCLK_GATE(vad_pdm_sysclk, AUDIO2_CLK_PDMIN_CTRL1);

struct clk_regmap aud_vad_vad_clk_sel =
	AUD_MST_MCLK_MUX(vad_vad_clk, AUDIO2_CLK_VAD_CTRL);
struct clk_regmap aud_vad_vad_clk_div =
	AUD_MST_MCLK_DIV(vad_vad_clk, AUDIO2_CLK_VAD_CTRL);
struct clk_regmap aud_vad_vad_clk =
	AUD_MST_MCLK_GATE(vad_vad_clk, AUDIO2_CLK_VAD_CTRL);

static const struct clk_parent_data aud_vad_mst_sclk_pdata[] = {
	{ .hw = &aud_vad_mst_vad_sclk.hw, .index = -1 },
	{ },
	{ },
	{ },
	{ },
	{ },
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

static const struct clk_parent_data aud_vad_mst_lrclk_pdata[] = {
	{ .hw = &aud_vad_mst_vad_lrclk.hw, .index = -1 },
	{ },
	{ },
	{ },
	{ },
	{ },
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

struct clk_regmap aud_vad_tdmin_vad_sclk_sel =
	AUD_TDM_SCLK_MUX(vad_tdmin_vad_a_sclk, AUDIO2_CLK_TDMIN_VAD_CTRL,
			 aud_vad_mst_sclk_pdata);
struct clk_regmap aud_vad_tdmin_vad_sclk_pre_en =
	AUD_TDM_SCLK_PRE_EN(tdmin_vad_sclk, AUDIO2_CLK_TDMIN_VAD_CTRL);
struct clk_regmap aud_vad_tdmin_vad_sclk_post_en =
	AUD_TDM_SCLK_POST_EN(tdmin_vad_sclk, AUDIO2_CLK_TDMIN_VAD_CTRL);
struct clk_regmap aud_vad_tdmin_vad_sclk =
	AUD_TDM_SCLK(tdmin_vad_sclk, AUDIO2_CLK_TDMIN_VAD_CTRL);
struct clk_regmap aud_vad_tdmin_vad_lrclk =
	AUD_TDM_LRLCK(tdmin_vad_lrclk, AUDIO2_CLK_TDMIN_VAD_CTRL,
		      aud_vad_mst_lrclk_pdata);

static struct clk_hw *a1_audio_vad_clkc_hws[] = {
	[AUD_VAD_CLKID_CLK81]		= &aud_vad_clk81.hw,
	[AUD_VAD_CLKID_SYSCLK_A_DIV]	= &aud_vad_sysclk_a_div.hw,
	[AUD_VAD_CLKID_SYSCLK_A]	= &aud_vad_sysclk_a.hw,
	[AUD_VAD_CLKID_SYSCLK_B_DIV]	= &aud_vad_sysclk_b_div.hw,
	[AUD_VAD_CLKID_SYSCLK_B]	= &aud_vad_sysclk_b.hw,
	[AUD_VAD_CLKID_SYSCLK]		= &aud_vad_sysclk.hw,
	[AUD_VAD_CLKID_DDR_ARB]		= &aud_vad_ddr_arb.hw,
	[AUD_VAD_CLKID_PDM]		= &aud_vad_pdm.hw,
	[AUD_VAD_CLKID_TDMIN_VAD]	= &aud_vad_tdmin_vad.hw,
	[AUD_VAD_CLKID_TODDR_VAD]	= &aud_vad_toddr_vad.hw,
	[AUD_VAD_CLKID_TOVAD]		= &aud_vad_tovad.hw,
	[AUD_VAD_CLKID_TOAUDIOTOP]	= &aud_vad_toaudiotop.hw,
	[AUD_VAD_CLKID_MST_VAD_MCLK_SEL] = &aud_vad_mst_vad_mclk_sel.hw,
	[AUD_VAD_CLKID_MST_VAD_MCLK_DIV] = &aud_vad_mst_vad_mclk_div.hw,
	[AUD_VAD_CLKID_MST_VAD_MCLK]	= &aud_vad_mst_vad_mclk.hw,
	[AUD_VAD_CLKID_MST_VAD_SCLK_PRE_EN] = &aud_vad_mst_vad_sclk_pre_en.hw,
	[AUD_VAD_CLKID_MST_VAD_SCLK_DIV] = &aud_vad_mst_vad_sclk_div.hw,
	[AUD_VAD_CLKID_MST_VAD_SCLK_POST_EN] = &aud_vad_mst_vad_sclk_post_en.hw,
	[AUD_VAD_CLKID_MST_VAD_SCLK]	= &aud_vad_mst_vad_sclk.hw,
	[AUD_VAD_CLKID_MST_VAD_LRCLK_DIV] = &aud_vad_mst_vad_lrclk_div.hw,
	[AUD_VAD_CLKID_MST_VAD_LRCLK]	= &aud_vad_mst_vad_lrclk.hw,
	[AUD_VAD_CLKID_TDMIN_VAD_SCLK_SEL] = &aud_vad_tdmin_vad_sclk_sel.hw,
	[AUD_VAD_CLKID_TDMIN_VAD_SCLK_PRE_EN] = &aud_vad_tdmin_vad_sclk_pre_en.hw,
	[AUD_VAD_CLKID_TDMIN_VAD_SCLK_POST_EN] = &aud_vad_tdmin_vad_sclk_post_en.hw,
	[AUD_VAD_CLKID_TDMIN_VAD_SCLK]	= &aud_vad_tdmin_vad_sclk.hw,
	[AUD_VAD_CLKID_TDMIN_VAD_LRCLK]	= &aud_vad_tdmin_vad_lrclk.hw,
	[AUD_VAD_CLKID_PDM_DCLK_SEL]	= &aud_vad_pdm_dclk_sel.hw,
	[AUD_VAD_CLKID_PDM_DCLK_DIV]	= &aud_vad_pdm_dclk_div.hw,
	[AUD_VAD_CLKID_PDM_DCLK]	= &aud_vad_pdm_dclk.hw,
	[AUD_VAD_CLKID_PDM_SYSCLK_SEL]	= &aud_vad_pdm_sysclk_sel.hw,
	[AUD_VAD_CLKID_PDM_SYSCLK_DIV]	= &aud_vad_pdm_sysclk_div.hw,
	[AUD_VAD_CLKID_PDM_SYSCLK]	= &aud_vad_pdm_sysclk.hw,
	[AUD_VAD_CLKID_VAD_CLK_SEL]	= &aud_vad_vad_clk_sel.hw,
	[AUD_VAD_CLKID_VAD_CLK_DIV]	= &aud_vad_vad_clk_div.hw,
	[AUD_VAD_CLKID_VAD_CLK]		= &aud_vad_vad_clk.hw,
};

struct a1_audio_data {
	struct meson_clk_hw_data hw_clks;
	const char *rst_drvname;
};

static const struct regmap_config a1_audio_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int a1_audio_clkc_probe(struct platform_device *pdev)
{
	const struct a1_audio_data *data;
	struct regmap *map;
	void __iomem *base;
	struct clk *clk;
	unsigned int i;
	int ret;

	data = device_get_match_data(&pdev->dev);
	if (!data)
		return -EINVAL;

	clk = devm_clk_get_enabled(&pdev->dev, "pclk");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	map = devm_regmap_init_mmio(&pdev->dev, base, &a1_audio_regmap_cfg);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ret = device_reset(&pdev->dev);
	if (ret)
		return ret;

	for (i = 0; i < data->hw_clks.num; i++) {
		struct clk_hw *hw = data->hw_clks.hws[i];
		struct clk_regmap *clk_regmap = to_clk_regmap(hw);

		if (!hw)
			continue;

		clk_regmap->map = map;

		ret = devm_clk_hw_register(&pdev->dev, hw);
		if (ret)
			return ret;
	}

	ret = devm_of_clk_add_hw_provider(&pdev->dev, meson_clk_hw_get,
					  (void *)&data->hw_clks);
	if (ret)
		return ret;

	return devm_meson_rst_aux_register(&pdev->dev, map, data->rst_drvname);
}

struct a1_audio_data a1_audio_clkc = {
	.hw_clks = {
		.hws = a1_audio_clkc_hws,
		.num = ARRAY_SIZE(a1_audio_clkc_hws),
	},
	.rst_drvname = "rst-a1",
};

struct a1_audio_data a1_audio_vad_clkc = {
	.hw_clks = {
		.hws = a1_audio_vad_clkc_hws,
		.num = ARRAY_SIZE(a1_audio_vad_clkc_hws),
	},
	.rst_drvname = "rst-a1-vad",
};

static const struct of_device_id a1_audio_clkc_match_table[] = {
	{
		.compatible = "amlogic,a1-audio-clkc",
		.data = &a1_audio_clkc,
	},
	{
		.compatible = "amlogic,a1-audio-vad-clkc",
		.data = &a1_audio_vad_clkc,
	},
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
MODULE_IMPORT_NS(CLK_MESON);
