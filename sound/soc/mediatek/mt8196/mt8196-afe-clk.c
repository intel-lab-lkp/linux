// SPDX-License-Identifier: GPL-2.0
/*
 *  mt8196-afe-clk.c  --  Mediatek 8196 afe clock ctrl
 *
 *  Copyright (c) 2024 MediaTek Inc.
 *  Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include "mt8196-afe-common.h"
#include "mt8196-audsys-clk.h"
#include "mt8196-afe-clk.h"

static const char *aud_clks[MT8196_CLK_NUM] = {
	/* vlp clk */
	[MT8196_CLK_VLP_MUX_AUDIOINTBUS] = "top_aud_intbus",
	[MT8196_CLK_VLP_MUX_AUD_ENG1] = "top_aud_eng1",
	[MT8196_CLK_VLP_MUX_AUD_ENG2] = "top_aud_eng2",
	[MT8196_CLK_VLP_MUX_AUDIO_H] = "top_aud_h",
	[MT8196_CLK_VLP_CLK26M] = "vlp_clk26m",
	/* pll */
	[MT8196_CLK_TOP_APLL1_CK] = "apll1",
	[MT8196_CLK_TOP_APLL2_CK] = "apll2",
	/* divider */
	[MT8196_CLK_TOP_APLL1_D4] = "apll1_d4",
	[MT8196_CLK_TOP_APLL2_D4] = "apll2_d4",
	[MT8196_CLK_TOP_APLL12_DIV_I2SIN0] = "apll12_div_i2sin0",
	[MT8196_CLK_TOP_APLL12_DIV_I2SIN1] = "apll12_div_i2sin1",
	[MT8196_CLK_TOP_APLL12_DIV_FMI2S] = "apll12_div_fmi2s",
	[MT8196_CLK_TOP_APLL12_DIV_TDMOUT_M] = "apll12_div_tdmout_m",
	[MT8196_CLK_TOP_APLL12_DIV_TDMOUT_B] = "apll12_div_tdmout_b",
	/* mux */
	[MT8196_CLK_TOP_MUX_AUD_1] = "top_apll1",
	[MT8196_CLK_TOP_MUX_AUD_2] = "top_apll2",
	[MT8196_CLK_TOP_I2SIN0_M_SEL] = "top_i2sin0",
	[MT8196_CLK_TOP_I2SIN1_M_SEL] = "top_i2sin1",
	[MT8196_CLK_TOP_FMI2S_M_SEL] = "top_fmi2s",
	[MT8196_CLK_TOP_TDMOUT_M_SEL] = "top_dptx",
	[MT8196_CLK_TOP_ADSP_SEL] = "top_adsp",
	/* top 26m*/
	[MT8196_CLK_TOP_CLK26M] = "clk26m",
	/* clock gate */
	[MT8196_CLK_AFE_AUDIO_HOPPING] = "afe_audio_hopping_ck",
	[MT8196_CLK_AFE_AUDIO_F26M] = "afe_audio_f26m_ck",
	[MT8196_CLK_AFE_APLL1] = "afe_apll1_ck",
	[MT8196_CLK_AFE_APLL2] = "afe_apll2_ck",
	[MT8196_CLK_AFE_APLL_TUNER1] = "afe_apll_tuner1",
	[MT8196_CLK_AFE_APLL_TUNER2] = "afe_apll_tuner2",
	[MT8196_CLK_AFE_ETDM_OUT4] = "afe_etdm_out4",
	[MT8196_CLK_AFE_ETDM_IN6] = "afe_etdm_in6",
	[MT8196_CLK_AFE_ETDM_OUT6] = "afe_etdm_out6",
	[MT8196_CLK_AFE_TDM_OUT] = "afe_tdm_out",
	[MT8196_CLK_AFE_CM0] = "afe_cm0",
	[MT8196_CLK_AFE_CM1] = "afe_cm1",
	[MT8196_CLK_AFE_CM2] = "afe_cm2",
};

int mt8196_afe_enable_clk(struct mtk_base_afe *afe, struct clk *clk)
{
	int ret;

	if (clk) {
		ret = clk_prepare_enable(clk);
		if (ret) {
			dev_dbg(afe->dev, "failed to enable clk\n");
			return ret;
		}
	} else {
		dev_dbg(afe->dev, "NULL clk\n");
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mt8196_afe_enable_clk);

void mt8196_afe_disable_clk(struct mtk_base_afe *afe, struct clk *clk)
{
	if (clk)
		clk_disable_unprepare(clk);
	else
		dev_dbg(afe->dev, "NULL clk\n");
}
EXPORT_SYMBOL_GPL(mt8196_afe_disable_clk);

static int mt8196_afe_set_clk_rate(struct mtk_base_afe *afe, struct clk *clk,
				   unsigned int rate)
{
	int ret;

	if (clk) {
		ret = clk_set_rate(clk, rate);
		if (ret) {
			dev_dbg(afe->dev, "failed to set clk rate\n");
			return ret;
		}
	}

	return 0;
}

static int mt8196_afe_set_clk_parent(struct mtk_base_afe *afe, struct clk *clk,
				     struct clk *parent)
{
	int ret;

	if (clk && parent) {
		ret = clk_set_parent(clk, parent);
		if (ret) {
			dev_dbg(afe->dev, "failed to set clk parent %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static unsigned int get_top_cg_reg(unsigned int cg_type)
{
	switch (cg_type) {
	case MT8196_AUDIO_26M_EN_ON:
	case MT8196_AUDIO_F3P25M_EN_ON:
	case MT8196_AUDIO_APLL1_EN_ON:
	case MT8196_AUDIO_APLL2_EN_ON:
		return AUDIO_ENGEN_CON0;
	default:
		return 0;
	}
}

static unsigned int get_top_cg_mask(unsigned int cg_type)
{
	switch (cg_type) {
	case MT8196_AUDIO_26M_EN_ON:
		return AUDIO_26M_EN_ON_MASK_SFT;
	case MT8196_AUDIO_F3P25M_EN_ON:
		return AUDIO_F3P25M_EN_ON_MASK_SFT;
	case MT8196_AUDIO_APLL1_EN_ON:
		return AUDIO_APLL1_EN_ON_MASK_SFT;
	case MT8196_AUDIO_APLL2_EN_ON:
		return AUDIO_APLL2_EN_ON_MASK_SFT;
	default:
		return 0;
	}
}

static unsigned int get_top_cg_on_val(unsigned int cg_type)
{
	switch (cg_type) {
	case MT8196_AUDIO_26M_EN_ON:
	case MT8196_AUDIO_F3P25M_EN_ON:
	case MT8196_AUDIO_APLL1_EN_ON:
	case MT8196_AUDIO_APLL2_EN_ON:
		return get_top_cg_mask(cg_type);
	default:
		return 0;
	}
}

static unsigned int get_top_cg_off_val(unsigned int cg_type)
{
	switch (cg_type) {
	case MT8196_AUDIO_26M_EN_ON:
	case MT8196_AUDIO_F3P25M_EN_ON:
	case MT8196_AUDIO_APLL1_EN_ON:
	case MT8196_AUDIO_APLL2_EN_ON:
		return 0;
	default:
		return get_top_cg_mask(cg_type);
	}
}

static int mt8196_afe_enable_top_cg(struct mtk_base_afe *afe, unsigned int cg_type)
{
	unsigned int reg = get_top_cg_reg(cg_type);
	unsigned int mask = get_top_cg_mask(cg_type);
	unsigned int val = get_top_cg_on_val(cg_type);

	regmap_update_bits(afe->regmap, reg, mask, val);
	return 0;
}

static int mt8196_afe_disable_top_cg(struct mtk_base_afe *afe, unsigned int cg_type)
{
	unsigned int reg = get_top_cg_reg(cg_type);
	unsigned int mask = get_top_cg_mask(cg_type);
	unsigned int val = get_top_cg_off_val(cg_type);

	regmap_update_bits(afe->regmap, reg, mask, val);
	return 0;
}

static int mt8196_afe_enable_afe_on(struct mtk_base_afe *afe)
{
	mt8196_afe_enable_top_cg(afe, MT8196_AUDIO_26M_EN_ON);
	return 0;
}

static int mt8196_afe_disable_afe_on(struct mtk_base_afe *afe)
{
	mt8196_afe_disable_top_cg(afe, MT8196_AUDIO_26M_EN_ON);
	return 0;
}

static int apll1_mux_setting(struct mtk_base_afe *afe, bool enable)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret = 0;

	dev_dbg(afe->dev, "enable: %d\n", enable);

	if (enable) {
		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1],
						afe_priv->clk[MT8196_CLK_TOP_APLL1_CK]);
		if (ret)
			return ret;

		/* 180.6336 / 4 = 45.1584MHz */
		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG1]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG1],
						afe_priv->clk[MT8196_CLK_TOP_APLL1_D4]);
		if (ret)
			return ret;

		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
						afe_priv->clk[MT8196_CLK_TOP_APLL1_CK]);
		if (ret)
			return ret;
	} else {
		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG1],
						afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
		if (ret)
			return ret;

		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG1]);

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1],
						afe_priv->clk[MT8196_CLK_TOP_CLK26M]);
		if (ret)
			return ret;

		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1]);
		mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
					  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	}

	return 0;
}

static int apll2_mux_setting(struct mtk_base_afe *afe, bool enable)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret = 0;

	dev_dbg(afe->dev, "enable: %d\n", enable);

	if (enable) {
		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2],
						afe_priv->clk[MT8196_CLK_TOP_APLL2_CK]);
		if (ret)
			return ret;

		/* 196.608 / 4 = 49.152MHz */
		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG2]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG2],
						afe_priv->clk[MT8196_CLK_TOP_APLL2_D4]);
		if (ret)
			return ret;

		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
						afe_priv->clk[MT8196_CLK_TOP_APLL2_CK]);
		if (ret)
			return ret;
	} else {
		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG2],
						afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
		if (ret)
			return ret;

		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUD_ENG2]);

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2],
						afe_priv->clk[MT8196_CLK_TOP_CLK26M]);
		if (ret)
			return ret;

		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2]);
		mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
					  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
		mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	}

	return 0;
}

static int mt8196_afe_disable_apll(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret = 0;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	if (ret)
		return ret;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1]);
	if (ret)
		goto clk_ck_mux_aud1_err;

	ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1],
					afe_priv->clk[MT8196_CLK_TOP_CLK26M]);
	if (ret)
		goto clk_ck_mux_aud1_parent_err;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2]);
	if (ret)
		goto clk_ck_mux_aud2_err;

	ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2],
					afe_priv->clk[MT8196_CLK_TOP_CLK26M]);
	if (ret)
		goto clk_ck_mux_aud2_parent_err;

	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2]);
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
				  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	return 0;

clk_ck_mux_aud2_parent_err:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_2]);
clk_ck_mux_aud2_err:
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1],
				  afe_priv->clk[MT8196_CLK_TOP_APLL1_CK]);
clk_ck_mux_aud1_parent_err:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_MUX_AUD_1]);
clk_ck_mux_aud1_err:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);

	return ret;
}

static void mt8196_afe_apll_init(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	if (afe_priv->vlp_ck) {
		regmap_write(afe_priv->vlp_ck, VLP_APLL1_TUNER_CON0, VLP_APLL1_TUNER_CON0_VALUE);
		regmap_write(afe_priv->vlp_ck, VLP_APLL2_TUNER_CON0, VLP_APLL2_TUNER_CON0_VALUE);
	} else {
		dev_warn(afe->dev, "vlp_ck regmap is null ptr\n");
	}
}

int mt8196_apll1_enable(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret;

	/* setting for APLL */
	apll1_mux_setting(afe, true);

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL1]);
	if (ret)
		goto ERR_CLK_APLL1;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER1]);
	if (ret)
		goto ERR_CLK_APLL1_TUNER;

	/* sel 44.1kHz:1, apll_div:7, upper bound:3 */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   XTAL_EN_128FS_SEL_MASK_SFT | APLL_DIV_MASK_SFT | UPPER_BOUND_MASK_SFT,
			   (0x1 << XTAL_EN_128FS_SEL_SFT) | (7 << APLL_DIV_SFT) |
			   (3 << UPPER_BOUND_SFT));

	/* apll1 freq tuner enable */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x1 << FREQ_TUNER_EN_SFT);

	/* audio apll1 on */
	mt8196_afe_enable_top_cg(afe, MT8196_AUDIO_APLL1_EN_ON);

	return 0;

ERR_CLK_APLL1_TUNER:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER1]);
ERR_CLK_APLL1:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL1]);
	return ret;
}

void mt8196_apll1_disable(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	/* audio apll1 off */
	mt8196_afe_disable_top_cg(afe, MT8196_AUDIO_APLL1_EN_ON);

	/* apll1 freq tuner disable */
	regmap_update_bits(afe->regmap, AFE_APLL1_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x0);

	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER1]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL1]);
	apll1_mux_setting(afe, false);
}

int mt8196_apll2_enable(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret;

	/* setting for APLL */
	apll2_mux_setting(afe, true);

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL2]);
	if (ret)
		goto ERR_CLK_APLL2;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER2]);
	if (ret)
		goto ERR_CLK_APLL2_TUNER;

	/* sel 48kHz: 2, apll_div: 7, upper bound: 3*/
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   XTAL_EN_128FS_SEL_MASK_SFT | APLL_DIV_MASK_SFT | UPPER_BOUND_MASK_SFT,
			   (0x2 << XTAL_EN_128FS_SEL_SFT) | (7 << APLL_DIV_SFT) |
			   (3 << UPPER_BOUND_SFT));

	/* apll2 freq tuner enable */
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x1 << FREQ_TUNER_EN_SFT);

	/* audio apll2 on */
	mt8196_afe_enable_top_cg(afe, MT8196_AUDIO_APLL2_EN_ON);
	return 0;

ERR_CLK_APLL2_TUNER:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER2]);
ERR_CLK_APLL2:
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL2]);
	return ret;

	return 0;
}

void mt8196_apll2_disable(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	/* audio apll2 off */
	mt8196_afe_disable_top_cg(afe, MT8196_AUDIO_APLL2_EN_ON);

	/* apll2 freq tuner disable */
	regmap_update_bits(afe->regmap, AFE_APLL2_TUNER_CFG,
			   FREQ_TUNER_EN_MASK_SFT,
			   0x0);

	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL_TUNER2]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_APLL2]);
	apll2_mux_setting(afe, false);
}

int mt8196_get_apll_rate(struct mtk_base_afe *afe, int apll)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int clk_id = 0;

	if (apll < MT8196_APLL1 || apll > MT8196_APLL2) {
		dev_warn(afe->dev, "invalid clk id\n");
		return 0;
	}

	if (apll == MT8196_APLL1)
		clk_id = MT8196_CLK_TOP_APLL1_CK;
	else
		clk_id = MT8196_CLK_TOP_APLL2_CK;

	return clk_get_rate(afe_priv->clk[clk_id]);
}

int mt8196_get_apll_by_rate(struct mtk_base_afe *afe, int rate)
{
	return ((rate % 8000) == 0) ? MT8196_APLL2 : MT8196_APLL1;
}

int mt8196_get_apll_by_name(struct mtk_base_afe *afe, const char *name)
{
	if (strcmp(name, APLL1_W_NAME) == 0)
		return MT8196_APLL1;
	else
		return MT8196_APLL2;
}

/* mck */
struct mt8196_mck_div {
	int m_sel_id;
	int div_clk_id;
};

static const struct mt8196_mck_div mck_div[MT8196_MCK_NUM] = {
	[MT8196_I2SIN0_MCK] = {
		.m_sel_id = MT8196_CLK_TOP_I2SIN0_M_SEL,
		.div_clk_id = MT8196_CLK_TOP_APLL12_DIV_I2SIN0,
	},
	[MT8196_I2SIN1_MCK] = {
		.m_sel_id = MT8196_CLK_TOP_I2SIN1_M_SEL,
		.div_clk_id = MT8196_CLK_TOP_APLL12_DIV_I2SIN1,
	},
	[MT8196_FMI2S_MCK] = {
		.m_sel_id = MT8196_CLK_TOP_FMI2S_M_SEL,
		.div_clk_id = MT8196_CLK_TOP_APLL12_DIV_FMI2S,
	},
	[MT8196_TDMOUT_MCK] = {
		.m_sel_id = MT8196_CLK_TOP_TDMOUT_M_SEL,
		.div_clk_id = MT8196_CLK_TOP_APLL12_DIV_TDMOUT_M,
	},
	[MT8196_TDMOUT_BCK] = {
		.m_sel_id = -1,
		.div_clk_id = MT8196_CLK_TOP_APLL12_DIV_TDMOUT_B,
	},
};

int mt8196_mck_enable(struct mtk_base_afe *afe, int mck_id, int rate)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int apll = mt8196_get_apll_by_rate(afe, rate);
	int apll_clk_id = apll == MT8196_APLL1 ?
			  MT8196_CLK_TOP_MUX_AUD_1 : MT8196_CLK_TOP_MUX_AUD_2;
	int m_sel_id = 0;
	int div_clk_id = 0;
	int ret = 0;

	dev_dbg(afe->dev, "mck_id: %d, rate: %d\n", mck_id, rate);

	if (mck_id >= MT8196_MCK_NUM || mck_id < 0)
		return -EINVAL;

	m_sel_id = mck_div[mck_id].m_sel_id;
	div_clk_id = mck_div[mck_id].div_clk_id;

	/* select apll */
	if (m_sel_id >= 0) {
		ret = mt8196_afe_enable_clk(afe, afe_priv->clk[m_sel_id]);
		if (ret)
			return ret;

		ret = mt8196_afe_set_clk_parent(afe, afe_priv->clk[m_sel_id],
						afe_priv->clk[apll_clk_id]);
		if (ret)
			return ret;
	}

	/* enable div, set rate */
	if (div_clk_id < 0) {
		dev_err(afe->dev, "invalid div_clk_id %d\n", div_clk_id);
		return -EINVAL;
	}
	if (div_clk_id == MT8196_CLK_TOP_APLL12_DIV_TDMOUT_B)
		rate = rate * 16;

	ret = mt8196_afe_enable_clk(afe, afe_priv->clk[div_clk_id]);
	if (ret)
		return ret;

	ret = mt8196_afe_set_clk_rate(afe, afe_priv->clk[div_clk_id], rate);
	if (ret)
		return ret;

	return 0;
}

int mt8196_mck_disable(struct mtk_base_afe *afe, int mck_id)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int m_sel_id = 0;
	int div_clk_id = 0;

	dev_dbg(afe->dev, "mck_id: %d.\n", mck_id);

	if (mck_id < 0) {
		dev_err(afe->dev, "mck_id = %d < 0\n", mck_id);
		return -EINVAL;
	}

	m_sel_id = mck_div[mck_id].m_sel_id;
	div_clk_id = mck_div[mck_id].div_clk_id;

	if (div_clk_id < 0) {
		dev_err(afe->dev, "div_clk_id = %d < 0\n",
			div_clk_id);
		return -EINVAL;
	}

	mt8196_afe_disable_clk(afe, afe_priv->clk[div_clk_id]);

	if (m_sel_id >= 0)
		mt8196_afe_disable_clk(afe, afe_priv->clk[m_sel_id]);

	return 0;
}

int mt8196_afe_enable_reg_rw_clk(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	/* bus clock for AFE external access, like DRAM */
	mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_ADSP_SEL]);

	/* bus clock for AFE internal access, like AFE SRAM */
	mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIOINTBUS]);
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIOINTBUS],
				  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
	/* enable audio vlp clock source */
	mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
				  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);

	/* AFE hw clock */
	/* IPM2.0: USE HOPPING & 26M */
	mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_AUDIO_HOPPING]);
	mt8196_afe_enable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_AUDIO_F26M]);
	return 0;
}

int mt8196_afe_disable_reg_rw_clk(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;

	/* IPM2.0: Use HOPPING & 26M */
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_AUDIO_HOPPING]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_AFE_AUDIO_F26M]);
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H],
				  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);

	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIO_H]);
	mt8196_afe_set_clk_parent(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIOINTBUS],
				  afe_priv->clk[MT8196_CLK_VLP_CLK26M]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_VLP_MUX_AUDIOINTBUS]);
	mt8196_afe_disable_clk(afe, afe_priv->clk[MT8196_CLK_TOP_ADSP_SEL]);
	return 0;
}

int mt8196_afe_enable_main_clock(struct mtk_base_afe *afe)
{
	mt8196_afe_enable_afe_on(afe);
	return 0;
}

int mt8196_afe_disable_main_clock(struct mtk_base_afe *afe)
{
	mt8196_afe_disable_afe_on(afe);
	return 0;
}

int mt8196_init_clock(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	int ret = 0;
	int i = 0;

	ret = mt8196_audsys_clk_register(afe);
	if (ret) {
		dev_err(afe->dev, "register audsys clk fail %d\n", ret);
		return ret;
	}

	afe_priv->clk = devm_kcalloc(afe->dev, MT8196_CLK_NUM, sizeof(*afe_priv->clk),
				     GFP_KERNEL);
	if (!afe_priv->clk)
		return -ENOMEM;

	for (i = 0; i < MT8196_CLK_NUM; i++) {
		afe_priv->clk[i] = devm_clk_get(afe->dev, aud_clks[i]);
		if (IS_ERR(afe_priv->clk[i])) {
			dev_err(afe->dev, "devm_clk_get %s fail\n", aud_clks[i]);
			return PTR_ERR(afe_priv->clk[i]);
		}
	}

	afe_priv->vlp_ck = syscon_regmap_lookup_by_phandle(afe->dev->of_node,
							   "vlpcksys");
	if (IS_ERR(afe_priv->vlp_ck)) {
		dev_err(afe->dev, "Cannot find vlpcksys\n");
		return PTR_ERR(afe_priv->vlp_ck);
	}

	mt8196_afe_apll_init(afe);

	ret = mt8196_afe_disable_apll(afe);
	if (ret)
		return ret;

	return 0;
}

