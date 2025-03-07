// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-mux.h"
#include "clk-pll.h"

/* MUX SEL REG */
#define VLP_CLK_CFG_UPDATE		0x0004
#define VLP_CLK_CFG_UPDATE1		0x0008
#define VLP_CLK_CFG_0			0x0010
#define VLP_CLK_CFG_0_SET		0x0014
#define VLP_CLK_CFG_0_CLR		0x0018
#define VLP_CLK_CFG_1			0x0020
#define VLP_CLK_CFG_1_SET		0x0024
#define VLP_CLK_CFG_1_CLR		0x0028
#define VLP_CLK_CFG_2			0x0030
#define VLP_CLK_CFG_2_SET		0x0034
#define VLP_CLK_CFG_2_CLR		0x0038
#define VLP_CLK_CFG_3			0x0040
#define VLP_CLK_CFG_3_SET		0x0044
#define VLP_CLK_CFG_3_CLR		0x0048
#define VLP_CLK_CFG_4			0x0050
#define VLP_CLK_CFG_4_SET		0x0054
#define VLP_CLK_CFG_4_CLR		0x0058
#define VLP_CLK_CFG_5			0x0060
#define VLP_CLK_CFG_5_SET		0x0064
#define VLP_CLK_CFG_5_CLR		0x0068
#define VLP_CLK_CFG_6			0x0070
#define VLP_CLK_CFG_6_SET		0x0074
#define VLP_CLK_CFG_6_CLR		0x0078
#define VLP_CLK_CFG_7			0x0080
#define VLP_CLK_CFG_7_SET		0x0084
#define VLP_CLK_CFG_7_CLR		0x0088
#define VLP_CLK_CFG_8			0x0090
#define VLP_CLK_CFG_8_SET		0x0094
#define VLP_CLK_CFG_8_CLR		0x0098
#define VLP_CLK_CFG_9			0x00a0
#define VLP_CLK_CFG_9_SET		0x00a4
#define VLP_CLK_CFG_9_CLR		0x00a8
#define VLP_CLK_CFG_10			0x00b0
#define VLP_CLK_CFG_10_SET		0x00b4
#define VLP_CLK_CFG_10_CLR		0x00b8
#define VLP_OCIC_FENC_STATUS_MON_0	0x039c
#define VLP_OCIC_FENC_STATUS_MON_1	0x03a0

/* MUX SHIFT */
#define TOP_MUX_SCP_SHIFT			0
#define TOP_MUX_SCP_SPI_SHIFT			1
#define TOP_MUX_SCP_IIC_SHIFT			2
#define TOP_MUX_SCP_IIC_HIGH_SPD_SHIFT		3
#define TOP_MUX_PWRAP_ULPOSC_SHIFT		4
#define TOP_MUX_SPMI_M_TIA_32K_SHIFT		5
#define TOP_MUX_APXGPT_26M_BCLK_SHIFT		6
#define TOP_MUX_DPSW_SHIFT			7
#define TOP_MUX_DPSW_CENTRAL_SHIFT		8
#define TOP_MUX_SPMI_M_MST_SHIFT		9
#define TOP_MUX_DVFSRC_SHIFT			10
#define TOP_MUX_PWM_VLP_SHIFT			11
#define TOP_MUX_AXI_VLP_SHIFT			12
#define TOP_MUX_SYSTIMER_26M_SHIFT		13
#define TOP_MUX_SSPM_SHIFT			14
#define TOP_MUX_SRCK_SHIFT			15
#define TOP_MUX_CAMTG0_SHIFT			16
#define TOP_MUX_CAMTG1_SHIFT			17
#define TOP_MUX_CAMTG2_SHIFT			18
#define TOP_MUX_CAMTG3_SHIFT			19
#define TOP_MUX_CAMTG4_SHIFT			20
#define TOP_MUX_CAMTG5_SHIFT			21
#define TOP_MUX_CAMTG6_SHIFT			22
#define TOP_MUX_CAMTG7_SHIFT			23
#define TOP_MUX_SSPM_26M_SHIFT			25
#define TOP_MUX_ULPOSC_SSPM_SHIFT		26
#define TOP_MUX_VLP_PBUS_26M_SHIFT		27
#define TOP_MUX_DEBUG_ERR_FLAG_VLP_26M_SHIFT	28
#define TOP_MUX_DPMSRDMA_SHIFT			29
#define TOP_MUX_VLP_PBUS_156M_SHIFT		30
#define TOP_MUX_SPM_SHIFT			0
#define TOP_MUX_MMINFRA_VLP_SHIFT		1
#define TOP_MUX_USB_TOP_SHIFT			2
#define TOP_MUX_SSUSB_XHCI_SHIFT		3
#define TOP_MUX_NOC_VLP_SHIFT			4
#define TOP_MUX_AUDIO_H_SHIFT			5
#define TOP_MUX_AUD_ENGEN1_SHIFT		6
#define TOP_MUX_AUD_ENGEN2_SHIFT		7
#define TOP_MUX_AUD_INTBUS_SHIFT		8
#define TOP_MUX_SPU_VLP_26M_SHIFT		9
#define TOP_MUX_SPU0_VLP_SHIFT			10
#define TOP_MUX_SPU1_VLP_SHIFT			11

/* CKSTA REG */
#define VLP_CKSTA_REG0			0x0250
#define VLP_CKSTA_REG1			0x0254

/* HW Voter REG */
#define VOTE_CG_9_SET	0x0048
#define VOTE_CG_9_CLR	0x004c
#define VOTE_CG_9_DONE	0x2c24
#define VOTE_CG_10_SET	0x0050
#define VOTE_CG_10_CLR	0x0054
#define VOTE_CG_10_DONE	0x2c28

/* PLL REG */
#define VLP_AP_PLL_CON3		0x264
#define VLP_APLL1_TUNER_CON0	0x2a4
#define VLP_APLL2_TUNER_CON0	0x2a8
#define VLP_APLL1_CON0		0x274
#define VLP_APLL1_CON1		0x278
#define VLP_APLL1_CON2		0x27c
#define VLP_APLL1_CON3		0x280
#define VLP_APLL2_CON0		0x28c
#define VLP_APLL2_CON1		0x290
#define VLP_APLL2_CON2		0x294
#define VLP_APLL2_CON3		0x298

#define MT8196_PLL_FMAX		(3800UL * MHZ)
#define MT8196_PLL_FMIN		(1500UL * MHZ)
#define MT8196_INTEGER_BITS	8

#define PLL_FENC(_id, _name, _reg, _fenc_sta_ofs, _fenc_sta_bit,\
			_flags, _pd_reg, _pd_shift,		\
			 _pcw_reg, _pcw_shift, _pcwbits) {	\
		.id = _id,					\
		.name = _name,					\
		.reg = _reg,					\
		.fenc_sta_ofs = _fenc_sta_ofs,			\
		.fenc_sta_bit = _fenc_sta_bit,			\
		.flags = (_flags) | CLK_FENC_ENABLE,		\
		.fmax = MT8196_PLL_FMAX,			\
		.fmin = MT8196_PLL_FMIN,			\
		.pd_reg = _pd_reg,				\
		.pd_shift = _pd_shift,				\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = _pcw_shift,			\
		.pcwbits = _pcwbits,				\
		.pcwibits = MT8196_INTEGER_BITS,		\
	}

static DEFINE_SPINLOCK(mt8196_clk_vlp_ck_lock);

static const struct mtk_fixed_factor vlp_ck_divs[] = {
	FACTOR(CLK_VLP_CK_OSC3, "vlp_osc3", "ulposc3", 1, 1),
	FACTOR(CLK_VLP_CK_CLKSQ, "vlp_clksq_ck", "clk26m", 1, 1),
	FACTOR(CLK_VLP_CK_AUDIO_H, "vlp_audio_h_ck", "vlp_audio_h_sel", 1, 1),
	FACTOR(CLK_VLP_CK_AUD_ENGEN1, "vlp_aud_engen1_ck", "vlp_aud_engen1_sel", 1, 1),
	FACTOR(CLK_VLP_CK_AUD_ENGEN2, "vlp_aud_engen2_ck", "vlp_aud_engen2_sel", 1, 1),
	FACTOR(CLK_VLP_CK_INFRA_26M, "vlp_infra_26m_ck", "ck_tck_26m_mx9_ck", 1, 1),
	FACTOR(CLK_VLP_CK_AUD_CLKSQ, "vlp_aud_clksq_ck", "vlp_clksq_ck", 1, 1),
};

static const char * const vlp_scp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d6",
	"ck_mainpll_d4",
	"ck_mainpll_d3",
	"ck_apll1_ck"
};

static const char * const vlp_scp_spi_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d5_d2"
};

static const char * const vlp_scp_iic_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d7_d2"
};

static const char * const vlp_scp_iic_hs_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d5_d4",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d7"
};

static const char * const vlp_pwrap_ulposc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_osc_d14",
	"ck_osc_d10"
};

static const char * const vlp_spmi_32ksel_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_clkrtc",
	"ck_osc_d20",
	"ck_osc_d14",
	"ck_osc_d10"
};

static const char * const vlp_apxgpt_26m_b_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_dpsw_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d7",
	"ck_mainpll_d7_d4"
};

static const char * const vlp_dpsw_central_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d10",
	"ck_osc_d7",
	"ck_mainpll_d7_d4"
};

static const char * const vlp_spmi_m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_osc_d14",
	"ck_osc_d10"
};

static const char * const vlp_dvfsrc_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_pwm_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_clkrtc",
	"ck_osc_d20",
	"ck_osc_d8",
	"ck_mainpll_d4_d8"
};

static const char * const vlp_axi_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d7_d4",
	"ck_osc_d4",
	"ck_mainpll_d7_d2"
};

static const char * const vlp_systimer_26m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_sspm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d5_d2",
	"ck_osc_d2",
	"ck_mainpll_d6"
};

static const char * const vlp_srck_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_camtg0_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc3",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc3",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg3_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg4_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg5_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg6_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_camtg7_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_univpll_192m_d32",
	"ck_univpll_192m_d16",
	"ck_f26m_d2",
	"ck_osc_d40",
	"ck_osc_d32",
	"ck_univpll_192m_d10",
	"ck_univpll_192m_d8",
	"ck_univpll_d6_d16",
	"ck_osc_d20",
	"ck2_tvdpll1_d16",
	"ck_univpll_d6_d8"
};

static const char * const vlp_sspm_26m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_ulposc_sspm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d2",
	"ck_mainpll_d4_d2"
};

static const char * const vlp_vlp_pbus_26m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_debug_err_flag_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_dpmsrdma_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d2"
};

static const char * const vlp_vlp_pbus_156m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d2",
	"ck_mainpll_d7_d2",
	"ck_mainpll_d7"
};

static const char * const vlp_spm_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d7_d4"
};

static const char * const vlp_mminfra_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d4",
	"ck_mainpll_d3"
};

static const char * const vlp_usb_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d9"
};

static const char * const vlp_usb_xhci_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_mainpll_d9"
};

static const char * const vlp_noc_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d9"
};

static const char * const vlp_audio_h_parents[] = {
	"ck_tck_26m_mx9_ck",
	"vlp_clksq_ck",
	"ck_apll1_ck",
	"ck_apll2_ck"
};

static const char * const vlp_aud_engen1_parents[] = {
	"ck_tck_26m_mx9_ck",
	"vlp_clksq_ck",
	"ck_apll1_d8",
	"ck_apll1_d4"
};

static const char * const vlp_aud_engen2_parents[] = {
	"ck_tck_26m_mx9_ck",
	"vlp_clksq_ck",
	"ck_apll2_d8",
	"ck_apll2_d4"
};

static const char * const vlp_aud_intbus_parents[] = {
	"ck_tck_26m_mx9_ck",
	"vlp_clksq_ck",
	"ck_mainpll_d7_d4",
	"ck_mainpll_d4_d4"
};

static const char * const vlp_spvlp_26m_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20"
};

static const char * const vlp_spu0_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const char * const vlp_spu1_vlp_parents[] = {
	"ck_tck_26m_mx9_ck",
	"ck_osc_d20",
	"ck_mainpll_d4_d4",
	"ck_mainpll_d4_d2",
	"ck_mainpll_d7",
	"ck_mainpll_d6",
	"ck_mainpll_d5"
};

static const struct mtk_mux vlp_ck_muxes[] = {
	/* VLP_CLK_CFG_0 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_SCP_SEL, "vlp_scp_sel", vlp_scp_parents,
		VLP_CLK_CFG_0, VLP_CLK_CFG_0_SET, VLP_CLK_CFG_0_CLR,
		0, 3, 7, VLP_CLK_CFG_UPDATE, TOP_MUX_SCP_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 31),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SCP_SPI_SEL, "vlp_scp_spi_sel",
		vlp_scp_spi_parents, VLP_CLK_CFG_0, VLP_CLK_CFG_0_SET,
		VLP_CLK_CFG_0_CLR, 8, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SCP_SPI_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SCP_IIC_SEL, "vlp_scp_iic_sel",
		vlp_scp_iic_parents, VLP_CLK_CFG_0, VLP_CLK_CFG_0_SET,
		VLP_CLK_CFG_0_CLR, 16, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SCP_IIC_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SCP_IIC_HIGH_SPD_SEL, "vlp_scp_iic_hs_sel",
		vlp_scp_iic_hs_parents, VLP_CLK_CFG_0, VLP_CLK_CFG_0_SET,
		VLP_CLK_CFG_0_CLR, 24, 3,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SCP_IIC_HIGH_SPD_SHIFT),
	/* VLP_CLK_CFG_1 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_PWRAP_ULPOSC_SEL, "vlp_pwrap_ulposc_sel",
		vlp_pwrap_ulposc_parents, VLP_CLK_CFG_1, VLP_CLK_CFG_1_SET,
		VLP_CLK_CFG_1_CLR, 0, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_PWRAP_ULPOSC_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPMI_M_TIA_32K_SEL, "vlp_spmi_32ksel",
		vlp_spmi_32ksel_parents, VLP_CLK_CFG_1, VLP_CLK_CFG_1_SET,
		VLP_CLK_CFG_1_CLR, 8, 3,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SPMI_M_TIA_32K_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_APXGPT_26M_BCLK_SEL, "vlp_apxgpt_26m_b_sel",
		vlp_apxgpt_26m_b_parents, VLP_CLK_CFG_1, VLP_CLK_CFG_1_SET,
		VLP_CLK_CFG_1_CLR, 16, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_APXGPT_26M_BCLK_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_DPSW_SEL, "vlp_dpsw_sel",
		vlp_dpsw_parents, VLP_CLK_CFG_1, VLP_CLK_CFG_1_SET,
		VLP_CLK_CFG_1_CLR, 24, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_DPSW_SHIFT),
	/* VLP_CLK_CFG_2 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_DPSW_CENTRAL_SEL, "vlp_dpsw_central_sel",
		vlp_dpsw_central_parents, VLP_CLK_CFG_2, VLP_CLK_CFG_2_SET,
		VLP_CLK_CFG_2_CLR, 0, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_DPSW_CENTRAL_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPMI_M_MST_SEL, "vlp_spmi_m_sel",
		vlp_spmi_m_parents, VLP_CLK_CFG_2, VLP_CLK_CFG_2_SET,
		VLP_CLK_CFG_2_CLR, 8, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SPMI_M_MST_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_DVFSRC_SEL, "vlp_dvfsrc_sel",
		vlp_dvfsrc_parents, VLP_CLK_CFG_2, VLP_CLK_CFG_2_SET,
		VLP_CLK_CFG_2_CLR, 16, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_DVFSRC_SHIFT),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_PWM_VLP_SEL, "vlp_pwm_vlp_sel", vlp_pwm_vlp_parents,
		VLP_CLK_CFG_2, VLP_CLK_CFG_2_SET, VLP_CLK_CFG_2_CLR,
		24, 3, 31, VLP_CLK_CFG_UPDATE, TOP_MUX_PWM_VLP_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 20),
	/* VLP_CLK_CFG_3 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_AXI_VLP_SEL, "vlp_axi_vlp_sel",
		vlp_axi_vlp_parents, VLP_CLK_CFG_3, VLP_CLK_CFG_3_SET,
		VLP_CLK_CFG_3_CLR, 0, 3,
		VLP_CLK_CFG_UPDATE, TOP_MUX_AXI_VLP_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SYSTIMER_26M_SEL, "vlp_systimer_26m_sel",
		vlp_systimer_26m_parents, VLP_CLK_CFG_3, VLP_CLK_CFG_3_SET,
		VLP_CLK_CFG_3_CLR, 8, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SYSTIMER_26M_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SSPM_SEL, "vlp_sspm_sel",
		vlp_sspm_parents, VLP_CLK_CFG_3, VLP_CLK_CFG_3_SET,
		VLP_CLK_CFG_3_CLR, 16, 3,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SSPM_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SRCK_SEL, "vlp_srck_sel",
		vlp_srck_parents, VLP_CLK_CFG_3, VLP_CLK_CFG_3_SET,
		VLP_CLK_CFG_3_CLR, 24, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SRCK_SHIFT),
	/* VLP_CLK_CFG_4 */
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG0_SEL, "vlp_camtg0_sel", vlp_camtg0_parents,
		VLP_CLK_CFG_4, VLP_CLK_CFG_4_SET, VLP_CLK_CFG_4_CLR, "vote-regmap",
		VOTE_CG_9_DONE, VOTE_CG_9_SET, VOTE_CG_9_CLR,
		0, 4, 7, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG0_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 15),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG1_SEL, "vlp_camtg1_sel", vlp_camtg1_parents,
		VLP_CLK_CFG_4, VLP_CLK_CFG_4_SET, VLP_CLK_CFG_4_CLR, "vote-regmap",
		VOTE_CG_9_DONE, VOTE_CG_9_SET, VOTE_CG_9_CLR,
		8, 4, 15, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG1_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 14),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG2_SEL, "vlp_camtg2_sel", vlp_camtg2_parents,
		VLP_CLK_CFG_4, VLP_CLK_CFG_4_SET, VLP_CLK_CFG_4_CLR, "vote-regmap",
		VOTE_CG_9_DONE, VOTE_CG_9_SET, VOTE_CG_9_CLR,
		16, 4, 23, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG2_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 13),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG3_SEL, "vlp_camtg3_sel", vlp_camtg3_parents,
		VLP_CLK_CFG_4, VLP_CLK_CFG_4_SET, VLP_CLK_CFG_4_CLR, "vote-regmap",
		VOTE_CG_9_DONE, VOTE_CG_9_SET, VOTE_CG_9_CLR,
		24, 4, 31, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG3_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 12),
	/* VLP_CLK_CFG_5 */
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG4_SEL, "vlp_camtg4_sel", vlp_camtg4_parents,
		VLP_CLK_CFG_5, VLP_CLK_CFG_5_SET, VLP_CLK_CFG_5_CLR, "vote-regmap",
		VOTE_CG_10_DONE, VOTE_CG_10_SET, VOTE_CG_10_CLR,
		0, 4, 7, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG4_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 11),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG5_SEL, "vlp_camtg5_sel", vlp_camtg5_parents,
		VLP_CLK_CFG_5, VLP_CLK_CFG_5_SET, VLP_CLK_CFG_5_CLR, "vote-regmap",
		VOTE_CG_10_DONE, VOTE_CG_10_SET, VOTE_CG_10_CLR,
		8, 4, 15, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG5_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 10),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG6_SEL, "vlp_camtg6_sel", vlp_camtg6_parents,
		VLP_CLK_CFG_5, VLP_CLK_CFG_5_SET, VLP_CLK_CFG_5_CLR, "vote-regmap",
		VOTE_CG_10_DONE, VOTE_CG_10_SET, VOTE_CG_10_CLR,
		16, 4, 23, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG6_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 9),
	MUX_MULT_VOTE_FENC(CLK_VLP_CK_CAMTG7_SEL, "vlp_camtg7_sel", vlp_camtg7_parents,
		VLP_CLK_CFG_5, VLP_CLK_CFG_5_SET, VLP_CLK_CFG_5_CLR, "vote-regmap",
		VOTE_CG_10_DONE, VOTE_CG_10_SET, VOTE_CG_10_CLR,
		24, 4, 31, VLP_CLK_CFG_UPDATE, TOP_MUX_CAMTG7_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_0, 8),
	/* VLP_CLK_CFG_6 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_SSPM_26M_SEL, "vlp_sspm_26m_sel",
		vlp_sspm_26m_parents, VLP_CLK_CFG_6, VLP_CLK_CFG_6_SET,
		VLP_CLK_CFG_6_CLR, 8, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_SSPM_26M_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_ULPOSC_SSPM_SEL, "vlp_ulposc_sspm_sel",
		vlp_ulposc_sspm_parents, VLP_CLK_CFG_6, VLP_CLK_CFG_6_SET,
		VLP_CLK_CFG_6_CLR, 16, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_ULPOSC_SSPM_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_VLP_PBUS_26M_SEL, "vlp_vlp_pbus_26m_sel",
		vlp_vlp_pbus_26m_parents, VLP_CLK_CFG_6, VLP_CLK_CFG_6_SET,
		VLP_CLK_CFG_6_CLR, 24, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_VLP_PBUS_26M_SHIFT),
	/* VLP_CLK_CFG_7 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_DEBUG_ERR_FLAG_SEL, "vlp_debug_err_flag_sel",
		vlp_debug_err_flag_parents, VLP_CLK_CFG_7, VLP_CLK_CFG_7_SET,
		VLP_CLK_CFG_7_CLR, 0, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_DEBUG_ERR_FLAG_VLP_26M_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_DPMSRDMA_SEL, "vlp_dpmsrdma_sel",
		vlp_dpmsrdma_parents, VLP_CLK_CFG_7, VLP_CLK_CFG_7_SET,
		VLP_CLK_CFG_7_CLR, 8, 1,
		VLP_CLK_CFG_UPDATE, TOP_MUX_DPMSRDMA_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_VLP_PBUS_156M_SEL, "vlp_vlp_pbus_156m_sel",
		vlp_vlp_pbus_156m_parents, VLP_CLK_CFG_7, VLP_CLK_CFG_7_SET,
		VLP_CLK_CFG_7_CLR, 16, 2,
		VLP_CLK_CFG_UPDATE, TOP_MUX_VLP_PBUS_156M_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPM_SEL, "vlp_spm_sel",
		vlp_spm_parents, VLP_CLK_CFG_7, VLP_CLK_CFG_7_SET,
		VLP_CLK_CFG_7_CLR, 24, 1,
		VLP_CLK_CFG_UPDATE1, TOP_MUX_SPM_SHIFT),
	/* VLP_CLK_CFG_8 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_MMINFRA_VLP_SEL, "vlp_mminfra_vlp_sel", vlp_mminfra_vlp_parents,
		VLP_CLK_CFG_8, VLP_CLK_CFG_8_SET, VLP_CLK_CFG_8_CLR,
		0, 2, 7, VLP_CLK_CFG_UPDATE1, TOP_MUX_MMINFRA_VLP_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 31),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_USB_TOP_SEL, "vlp_usb_sel", vlp_usb_parents,
		VLP_CLK_CFG_8, VLP_CLK_CFG_8_SET, VLP_CLK_CFG_8_CLR,
		8, 1, 15, VLP_CLK_CFG_UPDATE1, TOP_MUX_USB_TOP_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 30),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_USB_XHCI_SEL, "vlp_usb_xhci_sel", vlp_usb_xhci_parents,
		VLP_CLK_CFG_8, VLP_CLK_CFG_8_SET, VLP_CLK_CFG_8_CLR,
		16, 1, 23, VLP_CLK_CFG_UPDATE1, TOP_MUX_SSUSB_XHCI_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 29),
	MUX_CLR_SET_UPD(CLK_VLP_CK_NOC_VLP_SEL, "vlp_noc_vlp_sel",
		vlp_noc_vlp_parents, VLP_CLK_CFG_8, VLP_CLK_CFG_8_SET,
		VLP_CLK_CFG_8_CLR, 24, 2,
		VLP_CLK_CFG_UPDATE1, TOP_MUX_NOC_VLP_SHIFT),
	/* VLP_CLK_CFG_9 */
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_AUDIO_H_SEL, "vlp_audio_h_sel", vlp_audio_h_parents,
		VLP_CLK_CFG_9, VLP_CLK_CFG_9_SET, VLP_CLK_CFG_9_CLR,
		0, 2, 7, VLP_CLK_CFG_UPDATE1, TOP_MUX_AUDIO_H_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 27),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_AUD_ENGEN1_SEL, "vlp_aud_engen1_sel", vlp_aud_engen1_parents,
		VLP_CLK_CFG_9, VLP_CLK_CFG_9_SET, VLP_CLK_CFG_9_CLR,
		8, 2, 15, VLP_CLK_CFG_UPDATE1, TOP_MUX_AUD_ENGEN1_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 26),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_AUD_ENGEN2_SEL, "vlp_aud_engen2_sel", vlp_aud_engen2_parents,
		VLP_CLK_CFG_9, VLP_CLK_CFG_9_SET, VLP_CLK_CFG_9_CLR,
		16, 2, 23, VLP_CLK_CFG_UPDATE1, TOP_MUX_AUD_ENGEN2_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 25),
	MUX_GATE_FENC_CLR_SET_UPD(CLK_VLP_CK_AUD_INTBUS_SEL, "vlp_aud_intbus_sel", vlp_aud_intbus_parents,
		VLP_CLK_CFG_9, VLP_CLK_CFG_9_SET, VLP_CLK_CFG_9_CLR,
		24, 2, 31, VLP_CLK_CFG_UPDATE1, TOP_MUX_AUD_INTBUS_SHIFT,
		VLP_OCIC_FENC_STATUS_MON_1, 24),
	/* VLP_CLK_CFG_10 */
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPVLP_26M_SEL, "vlp_spvlp_26m_sel",
		vlp_spvlp_26m_parents, VLP_CLK_CFG_10, VLP_CLK_CFG_10_SET,
		VLP_CLK_CFG_10_CLR, 0, 1,
		VLP_CLK_CFG_UPDATE1, TOP_MUX_SPU_VLP_26M_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPU0_VLP_SEL, "vlp_spu0_vlp_sel",
		vlp_spu0_vlp_parents, VLP_CLK_CFG_10, VLP_CLK_CFG_10_SET,
		VLP_CLK_CFG_10_CLR, 8, 3,
		VLP_CLK_CFG_UPDATE1, TOP_MUX_SPU0_VLP_SHIFT),
	MUX_CLR_SET_UPD(CLK_VLP_CK_SPU1_VLP_SEL, "vlp_spu1_vlp_sel",
		vlp_spu1_vlp_parents, VLP_CLK_CFG_10, VLP_CLK_CFG_10_SET,
		VLP_CLK_CFG_10_CLR, 16, 3,
		VLP_CLK_CFG_UPDATE1, TOP_MUX_SPU1_VLP_SHIFT),
};

static const struct mtk_pll_data vlp_ck_plls[] = {
	PLL_FENC(CLK_VLP_CK_VLP_APLL1, "vlp_apll1", VLP_APLL1_CON0,
		 0x0358, 1, 0,
		 VLP_APLL1_CON1, 24,
		 VLP_APLL1_CON2, 0, 32),
	PLL_FENC(CLK_VLP_CK_VLP_APLL2, "vlp_apll2", VLP_APLL2_CON0,
		 0x0358, 0, 0,
		 VLP_APLL2_CON1, 24,
		 VLP_APLL2_CON2, 0, 32),
};

static int clk_mt8196_vlp_ck_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_VLP_CK_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_factors(vlp_ck_divs, ARRAY_SIZE(vlp_ck_divs), clk_data);
	if (r)
		goto free_clk_data;

	r = mtk_clk_register_muxes(&pdev->dev, vlp_ck_muxes, ARRAY_SIZE(vlp_ck_muxes), node,
				   &mt8196_clk_vlp_ck_lock, clk_data);
	if (r)
		goto unregister_factors;

	r = mtk_clk_register_plls(node, vlp_ck_plls, ARRAY_SIZE(vlp_ck_plls), clk_data);
	if (r)
		goto unregister_muxes;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_plls;

	return 0;

unregister_plls:
	mtk_clk_unregister_plls(vlp_ck_plls, ARRAY_SIZE(vlp_ck_plls), clk_data);
unregister_muxes:
	mtk_clk_unregister_muxes(vlp_ck_muxes, ARRAY_SIZE(vlp_ck_muxes), clk_data);
unregister_factors:
	mtk_clk_unregister_factors(vlp_ck_divs, ARRAY_SIZE(vlp_ck_divs), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);

	return r;
}

static void clk_mt8196_vlp_ck_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(vlp_ck_plls, ARRAY_SIZE(vlp_ck_plls), clk_data);
	mtk_clk_unregister_muxes(vlp_ck_muxes, ARRAY_SIZE(vlp_ck_muxes), clk_data);
	mtk_clk_unregister_factors(vlp_ck_divs, ARRAY_SIZE(vlp_ck_divs), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8196_vlp_ck[] = {
	{ .compatible = "mediatek,mt8196-vlp_cksys", },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_vlp_ck_drv = {
	.probe = clk_mt8196_vlp_ck_probe,
	.remove = clk_mt8196_vlp_ck_remove,
	.driver = {
		.name = "clk-mt8196-vlp_ck",
		.owner = THIS_MODULE,
		.of_match_table = of_match_clk_mt8196_vlp_ck,
	},
};

module_platform_driver(clk_mt8196_vlp_ck_drv);
MODULE_LICENSE("GPL");
