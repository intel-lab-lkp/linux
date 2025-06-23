// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 MediaTek Inc.
 *                    Guangjie Song <guangjie.song@mediatek.com>
 * Copyright (c) 2025 Collabora Ltd.
 *                    Laura Nao <laura.nao@collabora.com>
 */
#include <dt-bindings/clock/mediatek,mt8196-clock.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs afe0_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x0,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs afe1_cg_regs = {
	.set_ofs = 0x10,
	.clr_ofs = 0x10,
	.sta_ofs = 0x10,
};

static const struct mtk_gate_regs afe2_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x4,
	.sta_ofs = 0x4,
};

static const struct mtk_gate_regs afe3_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0x8,
	.sta_ofs = 0x8,
};

static const struct mtk_gate_regs afe4_cg_regs = {
	.set_ofs = 0xc,
	.clr_ofs = 0xc,
	.sta_ofs = 0xc,
};

#define GATE_AFE0(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &afe0_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

#define GATE_AFE1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &afe1_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

#define GATE_AFE2(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &afe2_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

#define GATE_AFE3(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &afe3_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

#define GATE_AFE4(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &afe4_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

static const struct mtk_gate afe_clks[] = {
	/* AFE0 */
	GATE_AFE0(CLK_AFE_PCM1, "afe_pcm1", "clk26m", 13),
	GATE_AFE0(CLK_AFE_PCM0, "afe_pcm0", "clk26m", 14),
	GATE_AFE0(CLK_AFE_CM2, "afe_cm2", "clk26m", 16),
	GATE_AFE0(CLK_AFE_CM1, "afe_cm1", "clk26m", 17),
	GATE_AFE0(CLK_AFE_CM0, "afe_cm0", "clk26m", 18),
	GATE_AFE0(CLK_AFE_STF, "afe_stf", "clk26m", 19),
	GATE_AFE0(CLK_AFE_HW_GAIN23, "afe_hw_gain23", "clk26m", 20),
	GATE_AFE0(CLK_AFE_HW_GAIN01, "afe_hw_gain01", "clk26m", 21),
	GATE_AFE0(CLK_AFE_FM_I2S, "afe_fm_i2s", "clk26m", 24),
	GATE_AFE0(CLK_AFE_MTKAIFV4, "afe_mtkaifv4", "clk26m", 25),
	/* AFE1 */
	GATE_AFE1(CLK_AFE_AUDIO_HOPPING, "afe_audio_hopping_ck", "clk26m", 0),
	GATE_AFE1(CLK_AFE_AUDIO_F26M, "afe_audio_f26m_ck", "clk26m", 1),
	GATE_AFE1(CLK_AFE_APLL1, "afe_apll1_ck", "aud_1", 2),
	GATE_AFE1(CLK_AFE_APLL2, "afe_apll2_ck", "aud_2", 3),
	GATE_AFE1(CLK_AFE_H208M, "afe_h208m_ck", "vlp_audio_h", 4),
	GATE_AFE1(CLK_AFE_APLL_TUNER2, "afe_apll_tuner2", "vlp_aud_engen2", 12),
	GATE_AFE1(CLK_AFE_APLL_TUNER1, "afe_apll_tuner1", "vlp_aud_engen1", 13),
	/* AFE2 */
	GATE_AFE2(CLK_AFE_UL2_ADC_HIRES_TML, "afe_ul2_aht", "vlp_audio_h", 12),
	GATE_AFE2(CLK_AFE_UL2_ADC_HIRES, "afe_ul2_adc_hires", "vlp_audio_h", 13),
	GATE_AFE2(CLK_AFE_UL2_TML, "afe_ul2_tml", "clk26m", 14),
	GATE_AFE2(CLK_AFE_UL2_ADC, "afe_ul2_adc", "clk26m", 15),
	GATE_AFE2(CLK_AFE_UL1_ADC_HIRES_TML, "afe_ul1_aht", "vlp_audio_h", 16),
	GATE_AFE2(CLK_AFE_UL1_ADC_HIRES, "afe_ul1_adc_hires", "vlp_audio_h", 17),
	GATE_AFE2(CLK_AFE_UL1_TML, "afe_ul1_tml", "clk26m", 18),
	GATE_AFE2(CLK_AFE_UL1_ADC, "afe_ul1_adc", "clk26m", 19),
	GATE_AFE2(CLK_AFE_UL0_ADC_HIRES_TML, "afe_ul0_aht", "vlp_audio_h", 20),
	GATE_AFE2(CLK_AFE_UL0_ADC_HIRES, "afe_ul0_adc_hires", "vlp_audio_h", 21),
	GATE_AFE2(CLK_AFE_UL0_TML, "afe_ul0_tml", "clk26m", 22),
	GATE_AFE2(CLK_AFE_UL0_ADC, "afe_ul0_adc", "clk26m", 23),
	/* AFE3 */
	GATE_AFE3(CLK_AFE_ETDM_IN6, "afe_etdm_in6", "clk26m", 7),
	GATE_AFE3(CLK_AFE_ETDM_IN5, "afe_etdm_in5", "clk26m", 8),
	GATE_AFE3(CLK_AFE_ETDM_IN4, "afe_etdm_in4", "clk26m", 9),
	GATE_AFE3(CLK_AFE_ETDM_IN3, "afe_etdm_in3", "clk26m", 10),
	GATE_AFE3(CLK_AFE_ETDM_IN2, "afe_etdm_in2", "clk26m", 11),
	GATE_AFE3(CLK_AFE_ETDM_IN1, "afe_etdm_in1", "clk26m", 12),
	GATE_AFE3(CLK_AFE_ETDM_IN0, "afe_etdm_in0", "clk26m", 13),
	GATE_AFE3(CLK_AFE_ETDM_OUT6, "afe_etdm_out6", "clk26m", 15),
	GATE_AFE3(CLK_AFE_ETDM_OUT5, "afe_etdm_out5", "clk26m", 16),
	GATE_AFE3(CLK_AFE_ETDM_OUT4, "afe_etdm_out4", "clk26m", 17),
	GATE_AFE3(CLK_AFE_ETDM_OUT3, "afe_etdm_out3", "clk26m", 18),
	GATE_AFE3(CLK_AFE_ETDM_OUT2, "afe_etdm_out2", "clk26m", 19),
	GATE_AFE3(CLK_AFE_ETDM_OUT1, "afe_etdm_out1", "clk26m", 20),
	GATE_AFE3(CLK_AFE_ETDM_OUT0, "afe_etdm_out0", "clk26m", 21),
	GATE_AFE3(CLK_AFE_TDM_OUT, "afe_tdm_out", "aud_1", 24),
	/* AFE4 */
	GATE_AFE4(CLK_AFE_GENERAL15_ASRC, "afe_general15_asrc", "clk26m", 9),
	GATE_AFE4(CLK_AFE_GENERAL14_ASRC, "afe_general14_asrc", "clk26m", 10),
	GATE_AFE4(CLK_AFE_GENERAL13_ASRC, "afe_general13_asrc", "clk26m", 11),
	GATE_AFE4(CLK_AFE_GENERAL12_ASRC, "afe_general12_asrc", "clk26m", 12),
	GATE_AFE4(CLK_AFE_GENERAL11_ASRC, "afe_general11_asrc", "clk26m", 13),
	GATE_AFE4(CLK_AFE_GENERAL10_ASRC, "afe_general10_asrc", "clk26m", 14),
	GATE_AFE4(CLK_AFE_GENERAL9_ASRC, "afe_general9_asrc", "clk26m", 15),
	GATE_AFE4(CLK_AFE_GENERAL8_ASRC, "afe_general8_asrc", "clk26m", 16),
	GATE_AFE4(CLK_AFE_GENERAL7_ASRC, "afe_general7_asrc", "clk26m", 17),
	GATE_AFE4(CLK_AFE_GENERAL6_ASRC, "afe_general6_asrc", "clk26m", 18),
	GATE_AFE4(CLK_AFE_GENERAL5_ASRC, "afe_general5_asrc", "clk26m", 19),
	GATE_AFE4(CLK_AFE_GENERAL4_ASRC, "afe_general4_asrc", "clk26m", 20),
	GATE_AFE4(CLK_AFE_GENERAL3_ASRC, "afe_general3_asrc", "clk26m", 21),
	GATE_AFE4(CLK_AFE_GENERAL2_ASRC, "afe_general2_asrc", "clk26m", 22),
	GATE_AFE4(CLK_AFE_GENERAL1_ASRC, "afe_general1_asrc", "clk26m", 23),
	GATE_AFE4(CLK_AFE_GENERAL0_ASRC, "afe_general0_asrc", "clk26m", 24),
	GATE_AFE4(CLK_AFE_CONNSYS_I2S_ASRC, "afe_connsys_i2s_asrc", "clk26m", 25),
};

static const struct mtk_clk_desc afe_mcd = {
	.clks = afe_clks,
	.num_clks = ARRAY_SIZE(afe_clks),
	.need_runtime_pm = true,
};

static const struct of_device_id of_match_clk_mt8196_adsp[] = {
	{ .compatible = "mediatek,mt8196-adsp", .data = &afe_mcd },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_adsp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-adsp",
		.of_match_table = of_match_clk_mt8196_adsp,
	},
};
module_platform_driver(clk_mt8196_adsp_drv);

MODULE_DESCRIPTION("MediaTek MT8196 AudioDSP clocks driver");
MODULE_LICENSE("GPL");
