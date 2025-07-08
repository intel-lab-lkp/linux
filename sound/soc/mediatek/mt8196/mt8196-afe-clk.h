/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt8196-afe-clk.h  --  Mediatek MT8196 AFE Clock Control definitions
 *
 * Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#ifndef _MT8196_AFE_CLOCK_CTRL_H_
#define _MT8196_AFE_CLOCK_CTRL_H_

/* vlp_cksys_clk: 0x1c016000 */
#define VLP_APLL1_TUNER_CON0 0x02a4
#define VLP_APLL2_TUNER_CON0 0x02a8

/* vlp apll1 tuner default value*/
#define VLP_APLL1_TUNER_CON0_VALUE 0x6f28bd4d
/* vlp apll2 tuner default value + 1*/
#define VLP_APLL2_TUNER_CON0_VALUE 0x78fd5265

/* APLL */
#define APLL1_W_NAME "APLL1"
#define APLL2_W_NAME "APLL2"

enum {
	MT8196_APLL1 = 0,
	MT8196_APLL2,
};

enum {
	/* vlp clk */
	MT8196_CLK_VLP_MUX_AUDIOINTBUS,
	MT8196_CLK_VLP_MUX_AUD_ENG1,
	MT8196_CLK_VLP_MUX_AUD_ENG2,
	MT8196_CLK_VLP_MUX_AUDIO_H,
	MT8196_CLK_VLP_CLK26M,
	/* pll */
	MT8196_CLK_TOP_APLL1_CK,
	MT8196_CLK_TOP_APLL2_CK,
	/* divider */
	MT8196_CLK_TOP_APLL1_D4,
	MT8196_CLK_TOP_APLL2_D4,
	MT8196_CLK_TOP_APLL12_DIV_I2SIN0,
	MT8196_CLK_TOP_APLL12_DIV_I2SIN1,
	MT8196_CLK_TOP_APLL12_DIV_FMI2S,
	MT8196_CLK_TOP_APLL12_DIV_TDMOUT_M,
	MT8196_CLK_TOP_APLL12_DIV_TDMOUT_B,
	/* mux */
	MT8196_CLK_TOP_MUX_AUD_1,
	MT8196_CLK_TOP_MUX_AUD_2,
	MT8196_CLK_TOP_I2SIN0_M_SEL,
	MT8196_CLK_TOP_I2SIN1_M_SEL,
	MT8196_CLK_TOP_FMI2S_M_SEL,
	MT8196_CLK_TOP_TDMOUT_M_SEL,
	MT8196_CLK_TOP_ADSP_SEL,
	/* top 26m */
	MT8196_CLK_TOP_CLK26M,
	MT8196_CLK_NUM,
};

struct mtk_base_afe;

int mt8196_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate);
int mt8196_mck_disable(struct mtk_base_afe *afe, int mck_id);
int mt8196_get_apll_rate(struct mtk_base_afe *afe, int apll);
int mt8196_get_apll_by_rate(struct mtk_base_afe *afe, int rate);
int mt8196_get_apll_by_name(struct mtk_base_afe *afe, const char *name);
int mt8196_init_clock(struct mtk_base_afe *afe);
int mt8196_afe_enable_clk(struct mtk_base_afe *afe, struct clk *clk);
void mt8196_afe_disable_clk(struct mtk_base_afe *afe, struct clk *clk);
int mt8196_apll1_enable(struct mtk_base_afe *afe);
void mt8196_apll1_disable(struct mtk_base_afe *afe);
int mt8196_apll2_enable(struct mtk_base_afe *afe);
void mt8196_apll2_disable(struct mtk_base_afe *afe);
int mt8196_afe_enable_main_clock(struct mtk_base_afe *afe);
int mt8196_afe_disable_main_clock(struct mtk_base_afe *afe);
int mt8196_afe_enable_reg_rw_clk(struct mtk_base_afe *afe);
int mt8196_afe_disable_reg_rw_clk(struct mtk_base_afe *afe);

#endif
