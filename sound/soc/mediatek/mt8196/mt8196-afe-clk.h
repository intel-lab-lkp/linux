/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt8196-afe-clk.h  --  Mediatek 8196 afe clock ctrl definition
 *
 * Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#ifndef _MT8196_AFE_CLOCK_CTRL_H_
#define _MT8196_AFE_CLOCK_CTRL_H_

/* vlp_cksys_clk: 0x1c016000 */
#define VLP_APLL1_TUNER_CON0 0x02a4
#define VLP_APLL2_TUNER_CON0 0x02a8

/* APLL */
#define APLL1_W_NAME "APLL1"
#define APLL2_W_NAME "APLL2"

enum {
	MT8196_APLL1 = 0,
	MT8196_APLL2,
};

enum {
	/* afe clk */
	CLK_HOPPING = 0,
	CLK_F26M,
	CLK_APLL1,
	CLK_APLL2,
	CLK_APLL1_TUNER,
	CLK_APLL2_TUNER,
	/* vlp clk */
	CLK_VLP_MUX_AUDIOINTBUS,
	CLK_VLP_MUX_AUD_ENG1,
	CLK_VLP_MUX_AUD_ENG2,
	CLK_VLP_MUX_AUDIO_H,
	CLK_VLP_CLK26M,
	/* ck clk */
	CLK_CK_MAINPLL_D4_D4,
	CLK_CK_MUX_AUD_1,
	CLK_CK_APLL1_CK,
	CLK_CK_MUX_AUD_2,
	CLK_CK_APLL2_CK,
	CLK_CK_APLL1_D4,
	CLK_CK_APLL2_D4,
	CLK_CK_I2SIN0_M_SEL,
	CLK_CK_I2SIN1_M_SEL,
	CLK_CK_FMI2S_M_SEL,
	CLK_CK_TDMOUT_M_SEL,
	CLK_CK_APLL12_DIV_I2SIN0,
	CLK_CK_APLL12_DIV_I2SIN1,
	CLK_CK_APLL12_DIV_FMI2S,
	CLK_CK_APLL12_DIV_TDMOUT_M,
	CLK_CK_APLL12_DIV_TDMOUT_B,
	CLK_CK_ADSP_SEL,
	CLK_CLK26M,
	CLK_NUM
};

struct mtk_base_afe;

int mt8196_init_clock(struct mtk_base_afe *afe);
int mt8196_afe_enable_clock(struct mtk_base_afe *afe);
void mt8196_afe_disable_clock(struct mtk_base_afe *afe);
int mt8196_afe_dram_request(struct device *dev);
int mt8196_afe_dram_release(struct device *dev);
int mt8196_apll1_enable(struct mtk_base_afe *afe);
void mt8196_apll1_disable(struct mtk_base_afe *afe);
int mt8196_apll2_enable(struct mtk_base_afe *afe);
void mt8196_apll2_disable(struct mtk_base_afe *afe);
int mt8196_get_apll_rate(struct mtk_base_afe *afe, int apll);
int mt8196_get_apll_by_rate(struct mtk_base_afe *afe, int rate);
int mt8196_get_apll_by_name(struct mtk_base_afe *afe, const char *name);
int mt8196_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate);
int mt8196_mck_disable(struct mtk_base_afe *afe, int mck_id);

#endif
