/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt8189-afe-clk.h  --  Mediatek 8189 afe clock ctrl definition
 *
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Darren Ye <darren.ye@mediatek.com>
 */

#ifndef _MT8189_AFE_CLOCK_CTRL_H_
#define _MT8189_AFE_CLOCK_CTRL_H_

#define APLL1_TUNER_CON0 0x0040
#define APLL2_TUNER_CON0 0x0044
/* apll1 tuner default value*/
#define APLL1_TUNER_CON0_VALUE 0x6f28bd4d
/* apll2 tuner default value + 1*/
#define APLL2_TUNER_CON0_VALUE 0x78fd5265

#define AP_PLL_CON3 0x000c
#define PLLEN_ALL 0x0070

#define APLL1_CON0 0x0334
#define APLL1_CON1 0x0338
#define APLL1_CON2 0x033c
#define APLL1_CON4 0x0344

#define APLL2_CON0 0x0348
#define APLL2_CON1 0x034c
#define APLL2_CON2 0x0350
#define APLL2_CON4 0x0358

#define CLK_CFG_6 0x0070
#define CLK_CFG_7 0x0080
#define CLK_CFG_9 0x00a0
#define CLK_CFG_10 0x00b0
#define CLK_CFG_11 0x00c0
#define CLK_CFG_12 0x00d0
#define CLK_CFG_13 0x00e0
#define CLK_CFG_UPDATE 0x004
#define CLK_CFG_UPDATE1 0x008

#define CLK_AUDDIV_0 0x0320
#define CLK_AUDDIV_1 0x0324
#define CLK_AUDDIV_2 0x0328
#define CLK_AUDDIV_3 0x0334
#define CLK_AUDDIV_4 0x0338
#define CLK_AUDDIV_5 0x033c

/* APLL */
#define APLL1_W_NAME "APLL1"
#define APLL2_W_NAME "APLL2"

enum {
	MT8189_APLL1 = 0,
	MT8189_APLL2,
};

enum {
	MT8189_CLK_TOP_MUX_AUDIOINTBUS,
	MT8189_CLK_TOP_MUX_AUD_ENG1,
	MT8189_CLK_TOP_MUX_AUD_ENG2,
	MT8189_CLK_TOP_MUX_AUDIO_H,
	/* pll */
	MT8189_CLK_TOP_APLL1_CK,
	MT8189_CLK_TOP_APLL2_CK,
	/* divider */
	MT8189_CLK_TOP_APLL1_D4,
	MT8189_CLK_TOP_APLL2_D4,
	MT8189_CLK_TOP_APLL12_DIV_I2SIN0,
	MT8189_CLK_TOP_APLL12_DIV_I2SIN1,
	MT8189_CLK_TOP_APLL12_DIV_FMI2S,
	MT8189_CLK_TOP_APLL12_DIV_TDMOUT_M,
	MT8189_CLK_TOP_APLL12_DIV_TDMOUT_B,
	/* mux */
	MT8189_CLK_TOP_MUX_AUD_1,
	MT8189_CLK_TOP_MUX_AUD_2,
	MT8189_CLK_TOP_I2SIN0_M_SEL,
	MT8189_CLK_TOP_I2SIN1_M_SEL,
	MT8189_CLK_TOP_FMI2S_M_SEL,
	MT8189_CLK_TOP_TDMOUT_M_SEL,
	/* top 26m */
	MT8189_CLK_TOP_CLK26M,
	/* peri */
	MT8189_CLK_PERAO_AUDIO_SLV_CK_PERI,
	MT8189_CLK_PERAO_AUDIO_MST_CK_PERI,
	MT8189_CLK_PERAO_INTBUS_CK_PERI,
	MT8189_CLK_NUM,
};

struct mtk_base_afe;

int mt8189_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate);
int mt8189_mck_disable(struct mtk_base_afe *afe, int mck_id);
int mt8189_get_apll_rate(struct mtk_base_afe *afe, int apll);
int mt8189_get_apll_by_rate(struct mtk_base_afe *afe, int rate);
int mt8189_get_apll_by_name(struct mtk_base_afe *afe, const char *name);
int mt8189_init_clock(struct mtk_base_afe *afe);
int mt8189_afe_enable_clk(struct mtk_base_afe *afe, struct clk *clk);
void mt8189_afe_disable_clk(struct mtk_base_afe *afe, struct clk *clk);
int mt8189_apll1_enable(struct mtk_base_afe *afe);
void mt8189_apll1_disable(struct mtk_base_afe *afe);
int mt8189_apll2_enable(struct mtk_base_afe *afe);
void mt8189_apll2_disable(struct mtk_base_afe *afe);
int mt8189_afe_enable_main_clock(struct mtk_base_afe *afe);
void mt8189_afe_disable_main_clock(struct mtk_base_afe *afe);
int mt8189_afe_enable_reg_rw_clk(struct mtk_base_afe *afe);
int mt8189_afe_disable_reg_rw_clk(struct mtk_base_afe *afe);

#endif
