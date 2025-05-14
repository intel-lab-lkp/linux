// SPDX-License-Identifier: GPL-2.0
/*
 * mt8196-audsys-clk.c  --  MediaTek 8196 audsys clock control
 *
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Darren Ye <darren.ye@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include "mt8196-afe-common.h"
#include "mt8196-audsys-clk.h"
#include "mt8196-audsys-clkid.h"
#include "mt8196-reg.h"

struct afe_gate {
	int id;
	const char *name;
	const char *parent_name;
	int reg;
	u8 bit;
	const struct clk_ops *ops;
	unsigned long flags;
	u8 cg_flags;
};

#define GATE_AFE_FLAGS(_id, _name, _parent, _reg, _bit, _flags, _cgflags) {\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.reg = _reg,					\
		.bit = _bit,					\
		.flags = _flags,				\
		.cg_flags = _cgflags,				\
	}

#define GATE_AFE(_id, _name, _parent, _reg, _bit)		\
	GATE_AFE_FLAGS(_id, _name, _parent, _reg, _bit,		\
		       CLK_SET_RATE_PARENT, CLK_GATE_SET_TO_DISABLE)

#define GATE_AUD0(_id, _name, _parent, _bit)			\
	GATE_AFE(_id, _name, _parent, AUDIO_TOP_CON0, _bit)

#define GATE_AUD1(_id, _name, _parent, _bit)			\
	GATE_AFE(_id, _name, _parent, AUDIO_TOP_CON1, _bit)

#define GATE_AUD2(_id, _name, _parent, _bit)			\
	GATE_AFE(_id, _name, _parent, AUDIO_TOP_CON2, _bit)

#define GATE_AUD3(_id, _name, _parent, _bit)			\
	GATE_AFE(_id, _name, _parent, AUDIO_TOP_CON3, _bit)

#define GATE_AUD4(_id, _name, _parent, _bit)			\
	GATE_AFE(_id, _name, _parent, AUDIO_TOP_CON4, _bit)

static const struct afe_gate aud_clks[CLK_AFE_NR_CLK] = {
	/* AFE0 */
	GATE_AUD0(CLK_AFE_PCM1, "afe_pcm1",
		  "vlp_aud_clksq_ck", 13),
	GATE_AUD0(CLK_AFE_PCM0, "afe_pcm0",
		  "vlp_aud_clksq_ck", 14),
	GATE_AUD0(CLK_AFE_CM2, "afe_cm2",
		  "vlp_aud_clksq_ck", 16),
	GATE_AUD0(CLK_AFE_CM1, "afe_cm1",
		  "vlp_aud_clksq_ck", 17),
	GATE_AUD0(CLK_AFE_CM0, "afe_cm0",
		  "vlp_aud_clksq_ck", 18),
	GATE_AUD0(CLK_AFE_STF, "afe_stf",
		  "vlp_aud_clksq_ck", 19),
	GATE_AUD0(CLK_AFE_HW_GAIN23, "afe_hw_gain23",
		  "vlp_aud_clksq_ck", 20),
	GATE_AUD0(CLK_AFE_HW_GAIN01, "afe_hw_gain01",
		  "vlp_aud_clksq_ck", 21),
	GATE_AUD0(CLK_AFE_FM_I2S, "afe_fm_i2s",
		  "vlp_aud_clksq_ck", 24),
	GATE_AUD0(CLK_AFE_MTKAIFV4, "afe_mtkaifv4",
		  "vlp_aud_clksq_ck", 25),
	/* AFE1 */
	GATE_AUD1(CLK_AFE_UL2_ADC_HIRES_TML, "afe_ul2_aht",
		  "vlp_audio_h_ck", 12),
	GATE_AUD1(CLK_AFE_UL2_ADC_HIRES, "afe_ul2_adc_hires",
		  "vlp_audio_h_ck", 13),
	GATE_AUD1(CLK_AFE_UL2_TML, "afe_ul2_tml",
		  "vlp_aud_clksq_ck", 14),
	GATE_AUD1(CLK_AFE_UL2_ADC, "afe_ul2_adc",
		  "vlp_aud_clksq_ck", 15),
	GATE_AUD1(CLK_AFE_UL1_ADC_HIRES_TML, "afe_ul1_aht",
		  "vlp_audio_h_ck", 16),
	GATE_AUD1(CLK_AFE_UL1_ADC_HIRES, "afe_ul1_adc_hires",
		  "vlp_audio_h_ck", 17),
	GATE_AUD1(CLK_AFE_UL1_TML, "afe_ul1_tml",
		  "vlp_aud_clksq_ck", 18),
	GATE_AUD1(CLK_AFE_UL1_ADC, "afe_ul1_adc",
		  "vlp_aud_clksq_ck", 19),
	GATE_AUD1(CLK_AFE_UL0_ADC_HIRES_TML, "afe_ul0_aht",
		  "vlp_audio_h_ck", 20),
	GATE_AUD1(CLK_AFE_UL0_ADC_HIRES, "afe_ul0_adc_hires",
		  "vlp_audio_h_ck", 21),
	GATE_AUD1(CLK_AFE_UL0_TML, "afe_ul0_tml",
		  "vlp_aud_clksq_ck", 22),
	GATE_AUD1(CLK_AFE_UL0_ADC, "afe_ul0_adc",
		  "vlp_aud_clksq_ck", 23),
	/* AFE2 */
	GATE_AUD2(CLK_AFE_ETDM_IN6, "afe_etdm_in6",
		  "vlp_aud_clksq_ck", 7),
	GATE_AUD2(CLK_AFE_ETDM_IN5, "afe_etdm_in5",
		  "vlp_aud_clksq_ck", 8),
	GATE_AUD2(CLK_AFE_ETDM_IN4, "afe_etdm_in4",
		  "vlp_aud_clksq_ck", 9),
	GATE_AUD2(CLK_AFE_ETDM_IN3, "afe_etdm_in3",
		  "vlp_aud_clksq_ck", 10),
	GATE_AUD2(CLK_AFE_ETDM_IN2, "afe_etdm_in2",
		  "vlp_aud_clksq_ck", 11),
	GATE_AUD2(CLK_AFE_ETDM_IN1, "afe_etdm_in1",
		  "vlp_aud_clksq_ck", 12),
	GATE_AUD2(CLK_AFE_ETDM_IN0, "afe_etdm_in0",
		  "vlp_aud_clksq_ck", 13),
	GATE_AUD2(CLK_AFE_ETDM_OUT6, "afe_etdm_out6",
		  "vlp_aud_clksq_ck", 15),
	GATE_AUD2(CLK_AFE_ETDM_OUT5, "afe_etdm_out5",
		  "vlp_aud_clksq_ck", 16),
	GATE_AUD2(CLK_AFE_ETDM_OUT4, "afe_etdm_out4",
		  "vlp_aud_clksq_ck", 17),
	GATE_AUD2(CLK_AFE_ETDM_OUT3, "afe_etdm_out3",
		  "vlp_aud_clksq_ck", 18),
	GATE_AUD2(CLK_AFE_ETDM_OUT2, "afe_etdm_out2",
		  "vlp_aud_clksq_ck", 19),
	GATE_AUD2(CLK_AFE_ETDM_OUT1, "afe_etdm_out1",
		  "vlp_aud_clksq_ck", 20),
	GATE_AUD2(CLK_AFE_ETDM_OUT0, "afe_etdm_out0",
		  "vlp_aud_clksq_ck", 21),
	GATE_AUD2(CLK_AFE_TDM_OUT, "afe_tdm_out",
		  "ck_aud_1_ck", 24),
	/* AFE3 */
	GATE_AUD3(CLK_AFE_GENERAL15_ASRC, "afe_general15_asrc",
		  "vlp_aud_clksq_ck", 9),
	GATE_AUD3(CLK_AFE_GENERAL14_ASRC, "afe_general14_asrc",
		  "vlp_aud_clksq_ck", 10),
	GATE_AUD3(CLK_AFE_GENERAL13_ASRC, "afe_general13_asrc",
		  "vlp_aud_clksq_ck", 11),
	GATE_AUD3(CLK_AFE_GENERAL12_ASRC, "afe_general12_asrc",
		  "vlp_aud_clksq_ck", 12),
	GATE_AUD3(CLK_AFE_GENERAL11_ASRC, "afe_general11_asrc",
		  "vlp_aud_clksq_ck", 13),
	GATE_AUD3(CLK_AFE_GENERAL10_ASRC, "afe_general10_asrc",
		  "vlp_aud_clksq_ck", 14),
	GATE_AUD3(CLK_AFE_GENERAL9_ASRC, "afe_general9_asrc",
		  "vlp_aud_clksq_ck", 15),
	GATE_AUD3(CLK_AFE_GENERAL8_ASRC, "afe_general8_asrc",
		  "vlp_aud_clksq_ck", 16),
	GATE_AUD3(CLK_AFE_GENERAL7_ASRC, "afe_general7_asrc",
		  "vlp_aud_clksq_ck", 17),
	GATE_AUD3(CLK_AFE_GENERAL6_ASRC, "afe_general6_asrc",
		  "vlp_aud_clksq_ck", 18),
	GATE_AUD3(CLK_AFE_GENERAL5_ASRC, "afe_general5_asrc",
		  "vlp_aud_clksq_ck", 19),
	GATE_AUD3(CLK_AFE_GENERAL4_ASRC, "afe_general4_asrc",
		  "vlp_aud_clksq_ck", 20),
	GATE_AUD3(CLK_AFE_GENERAL3_ASRC, "afe_general3_asrc",
		  "vlp_aud_clksq_ck", 21),
	GATE_AUD3(CLK_AFE_GENERAL2_ASRC, "afe_general2_asrc",
		  "vlp_aud_clksq_ck", 22),
	GATE_AUD3(CLK_AFE_GENERAL1_ASRC, "afe_general1_asrc",
		  "vlp_aud_clksq_ck", 23),
	GATE_AUD3(CLK_AFE_GENERAL0_ASRC, "afe_general0_asrc",
		  "vlp_aud_clksq_ck", 24),
	GATE_AUD3(CLK_AFE_CONNSYS_I2S_ASRC, "afe_connsys_i2s_asrc",
		  "vlp_aud_clksq_ck", 25),
	/* AFE4 */
	GATE_AUD4(CLK_AFE_AUDIO_HOPPING, "afe_audio_hopping_ck",
		  "vlp_aud_clksq_ck", 0),
	GATE_AUD4(CLK_AFE_AUDIO_F26M, "afe_audio_f26m_ck",
		  "vlp_aud_clksq_ck", 1),
	GATE_AUD4(CLK_AFE_APLL1, "afe_apll1_ck",
		  "ck_aud_1_ck", 2),
	GATE_AUD4(CLK_AFE_APLL2, "afe_apll2_ck",
		  "ck_aud_2_ck", 3),
	GATE_AUD4(CLK_AFE_H208M, "afe_h208m_ck",
		  "vlp_audio_h_ck", 4),
	GATE_AUD4(CLK_AFE_APLL_TUNER2, "afe_apll_tuner2",
		  "vlp_aud_engen2_ck", 12),
	GATE_AUD4(CLK_AFE_APLL_TUNER1, "afe_apll_tuner1",
		  "vlp_aud_engen1_ck", 13),
};

static void mt8196_audsys_clk_unregister(void *data)
{
	struct mtk_base_afe *afe = data;
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	struct clk *clk;
	struct clk_lookup *cl;
	int i;

	if (!afe_priv)
		return;

	for (i = 0; i < CLK_AFE_NR_CLK; i++) {
		cl = afe_priv->lookup[i];
		if (!cl)
			continue;

		clk = cl->clk;
		clk_unregister_gate(clk);

		clkdev_drop(cl);
	}
}

int mt8196_audsys_clk_register(struct mtk_base_afe *afe)
{
	struct mt8196_afe_private *afe_priv = afe->platform_priv;
	struct clk *clk;
	struct clk_lookup *cl;
	int i;

	afe_priv->lookup = devm_kcalloc(afe->dev, CLK_AFE_NR_CLK,
					sizeof(*afe_priv->lookup),
					GFP_KERNEL);

	if (!afe_priv->lookup)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(aud_clks); i++) {
		const struct afe_gate *gate = &aud_clks[i];

		clk = clk_register_gate(afe->dev, gate->name, gate->parent_name,
					gate->flags, afe->base_addr + gate->reg,
					gate->bit, gate->cg_flags, NULL);

		if (IS_ERR(clk)) {
			dev_err(afe->dev, "Failed to register clk %s: %ld\n",
				gate->name, PTR_ERR(clk));
			continue;
		}

		/* add clk_lookup for devm_clk_get(SND_SOC_DAPM_CLOCK_SUPPLY) */
		cl = kzalloc(sizeof(*cl), GFP_KERNEL);
		if (!cl)
			return -ENOMEM;

		cl->clk = clk;
		cl->con_id = gate->name;
		cl->dev_id = dev_name(afe->dev);
		cl->clk_hw = NULL;
		clkdev_add(cl);

		afe_priv->lookup[i] = cl;
	}

	return devm_add_action_or_reset(afe->dev, mt8196_audsys_clk_unregister, afe);
}
