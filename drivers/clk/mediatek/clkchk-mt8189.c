// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#include <dt-bindings/power/mt8189-power.h>

#if IS_ENABLED(CONFIG_MTK_DVFSRC_HELPER)
#include <mt-plat/dvfsrc-exp.h>
#endif

#include "clkchk.h"
#include "clkchk-mt8189.h"
#include "clk-fmeter.h"
#include "clk-mt8189-fmeter.h"

#define BUG_ON_CHK_ENABLE		0
#define CHECK_VCORE_FREQ		0
#define CG_CHK_PWRON_ENABLE		0

#define EVT_LEN				40
#define CLK_ID_SHIFT			0
#define CLK_STA_SHIFT			8

#define INV_MSK				0xFFFFFFFF

static DEFINE_SPINLOCK(clk_trace_lock);
static unsigned int clk_event[EVT_LEN];
static unsigned int evt_cnt, suspend_cnt;

/* trace all subsys cgs */
enum {
	CLK_AFE_DL0_DAC_TML_CG = 0,
	CLK_AFE_DL0_DAC_HIRES_CG = 1,
	CLK_AFE_DL0_DAC_CG = 2,
	CLK_AFE_DL0_PREDIS_CG = 3,
	CLK_AFE_DL0_NLE_CG = 4,
	CLK_AFE_PCM0_CG = 5,
	CLK_AFE_CM1_CG = 6,
	CLK_AFE_CM0_CG = 7,
	CLK_AFE_HW_GAIN23_CG = 8,
	CLK_AFE_HW_GAIN01_CG = 9,
	CLK_AFE_FM_I2S_CG = 10,
	CLK_AFE_MTKAIFV4_CG = 11,
	CLK_AFE_AUDIO_HOPPING_CG = 12,
	CLK_AFE_AUDIO_F26M_CG = 13,
	CLK_AFE_APLL1_CG = 14,
	CLK_AFE_APLL2_CG = 15,
	CLK_AFE_H208M_CG = 16,
	CLK_AFE_APLL_TUNER2_CG = 17,
	CLK_AFE_APLL_TUNER1_CG = 18,
	CLK_AFE_DMIC1_ADC_HIRES_TML_CG = 19,
	CLK_AFE_DMIC1_ADC_HIRES_CG = 20,
	CLK_AFE_DMIC1_TML_CG = 21,
	CLK_AFE_DMIC1_ADC_CG = 22,
	CLK_AFE_DMIC0_ADC_HIRES_TML_CG = 23,
	CLK_AFE_DMIC0_ADC_HIRES_CG = 24,
	CLK_AFE_DMIC0_TML_CG = 25,
	CLK_AFE_DMIC0_ADC_CG = 26,
	CLK_AFE_UL0_ADC_HIRES_TML_CG = 27,
	CLK_AFE_UL0_ADC_HIRES_CG = 28,
	CLK_AFE_UL0_TML_CG = 29,
	CLK_AFE_UL0_ADC_CG = 30,
	CLK_AFE_ETDM_IN1_CG = 31,
	CLK_AFE_ETDM_IN0_CG = 32,
	CLK_AFE_ETDM_OUT4_CG = 33,
	CLK_AFE_ETDM_OUT1_CG = 34,
	CLK_AFE_ETDM_OUT0_CG = 35,
	CLK_AFE_TDM_OUT_CG = 36,
	CLK_AFE_GENERAL4_ASRC_CG = 37,
	CLK_AFE_GENERAL3_ASRC_CG = 38,
	CLK_AFE_GENERAL2_ASRC_CG = 39,
	CLK_AFE_GENERAL1_ASRC_CG = 40,
	CLK_AFE_GENERAL0_ASRC_CG = 41,
	CLK_AFE_CONNSYS_I2S_ASRC_CG = 42,
	CLK_VAD_CORE0_EN_CG = 43,
	CLK_VAD_BUSEMI_EN_CG = 44,
	CLK_VAD_TIMER_EN_CG = 45,
	CLK_VAD_DMA0_EN_CG = 46,
	CLK_VAD_UART_EN_CG = 47,
	CLK_VAD_VOWPLL_EN_CG = 48,
	CLK_VADSYS_26M_CG = 49,
	CLK_VADSYS_BUS_CG = 50,
	CLK_CAM_M_LARB13_CG = 51,
	CLK_CAM_M_LARB14_CG = 52,
	CLK_CAM_M_CAMSYS_MAIN_CAM_CG = 53,
	CLK_CAM_M_CAMSYS_MAIN_CAMTG_CG = 54,
	CLK_CAM_M_SENINF_CG = 55,
	CLK_CAM_M_CAMSV1_CG = 56,
	CLK_CAM_M_CAMSV2_CG = 57,
	CLK_CAM_M_CAMSV3_CG = 58,
	CLK_CAM_M_FAKE_ENG_CG = 59,
	CLK_CAM_M_CAM2MM_GALS_CG = 60,
	CLK_CAM_M_CAMSV4_CG = 61,
	CLK_CAM_M_PDA_CG = 62,
	CLK_CAM_RA_CAMSYS_RAWA_LARBX_CG = 63,
	CLK_CAM_RA_CAMSYS_RAWA_CAM_CG = 64,
	CLK_CAM_RA_CAMSYS_RAWA_CAMTG_CG = 65,
	CLK_CAM_RB_CAMSYS_RAWB_LARBX_CG = 66,
	CLK_CAM_RB_CAMSYS_RAWB_CAM_CG = 67,
	CLK_CAM_RB_CAMSYS_RAWB_CAMTG_CG = 68,
	CLK_MM_DISP_OVL0_4L_CG = 69,
	CLK_MM_DISP_OVL1_4L_CG = 70,
	CLK_MM_VPP_RSZ0_CG = 71,
	CLK_MM_VPP_RSZ1_CG = 72,
	CLK_MM_DISP_RDMA0_CG = 73,
	CLK_MM_DISP_RDMA1_CG = 74,
	CLK_MM_DISP_COLOR0_CG = 75,
	CLK_MM_DISP_COLOR1_CG = 76,
	CLK_MM_DISP_CCORR0_CG = 77,
	CLK_MM_DISP_CCORR1_CG = 78,
	CLK_MM_DISP_CCORR2_CG = 79,
	CLK_MM_DISP_CCORR3_CG = 80,
	CLK_MM_DISP_AAL0_CG = 81,
	CLK_MM_DISP_AAL1_CG = 82,
	CLK_MM_DISP_GAMMA0_CG = 83,
	CLK_MM_DISP_GAMMA1_CG = 84,
	CLK_MM_DISP_DITHER0_CG = 85,
	CLK_MM_DISP_DITHER1_CG = 86,
	CLK_MM_DISP_DSC_WRAP0_CG = 87,
	CLK_MM_VPP_MERGE0_CG = 88,
	CLK_MMSYS_0_DISP_DVO_CG = 89,
	CLK_MMSYS_0_DISP_DSI0_CG = 90,
	CLK_MM_DP_INTF0_CG = 91,
	CLK_MM_DISP_WDMA0_CG = 92,
	CLK_MM_DISP_WDMA1_CG = 93,
	CLK_MM_DISP_FAKE_ENG0_CG = 94,
	CLK_MM_DISP_FAKE_ENG1_CG = 95,
	CLK_MM_SMI_LARB_CG = 96,
	CLK_MM_DISP_MUTEX0_CG = 97,
	CLK_MM_DIPSYS_CONFIG_CG = 98,
	CLK_MM_DUMMY_CG = 99,
	CLK_MMSYS_1_DISP_DSI0_CG = 100,
	CLK_MMSYS_1_LVDS_ENCODER_CG = 101,
	CLK_MMSYS_1_DISP_DVO_CG = 102,
	CLK_MM_DP_INTF_CG = 103,
	CLK_MMSYS_1_LVDS_ENCODER_CTS_CG = 104,
	CLK_MMSYS_1_DISP_DVO_AVT_CG = 105,
	CLK_GCE_D_TOP_CG = 106,
	CLK_GCE_M_TOP_CG = 107,
	CLK_MMINFRA_GCE_D_CG = 108,
	CLK_MMINFRA_GCE_M_CG = 109,
	CLK_MMINFRA_SMI_CG = 110,
	CLK_MMINFRA_GCE_26M_CG = 111,
	CLK_IMGSYS1_LARB9_CG = 112,
	CLK_IMGSYS1_LARB11_CG = 113,
	CLK_IMGSYS1_DIP_CG = 114,
	CLK_IMGSYS1_GALS_CG = 115,
	CLK_IMGSYS2_LARB9_CG = 116,
	CLK_IMGSYS2_LARB11_CG = 117,
	CLK_IMGSYS2_MFB_CG = 118,
	CLK_IMGSYS2_WPE_CG = 119,
	CLK_IMGSYS2_MSS_CG = 120,
	CLK_IMGSYS2_GALS_CG = 121,
	CLK_IPE_LARB19_CG = 122,
	CLK_IPE_LARB20_CG = 123,
	CLK_IPE_SMI_SUBCOM_CG = 124,
	CLK_IPE_FD_CG = 125,
	CLK_IPE_FE_CG = 126,
	CLK_IPE_RSC_CG = 127,
	CLK_IPESYS_GALS_CG = 128,
	CLK_IMPE_I2C0_CG = 129,
	CLK_IMPE_I2C1_CG = 130,
	CLK_IMPEN_I2C7_CG = 131,
	CLK_IMPEN_I2C8_CG = 132,
	CLK_IMPS_I2C3_CG = 133,
	CLK_IMPS_I2C4_CG = 134,
	CLK_IMPS_I2C5_CG = 135,
	CLK_IMPS_I2C6_CG = 136,
	CLK_IMPWS_I2C2_CG = 137,
	CLK_IFRAO_CQ_DMA_FPC_CG = 138,
	CLK_IFRAO_DEBUGSYS_CG = 139,
	CLK_IFRAO_DBG_TRACE_CG = 140,
	CLK_IFRAO_CQ_DMA_CG = 141,
	CLK_PERAO_UART0_CG = 142,
	CLK_PERAO_UART1_CG = 143,
	CLK_PERAO_UART2_CG = 144,
	CLK_PERAO_UART3_CG = 145,
	CLK_PERAO_PWM_H_CG = 146,
	CLK_PERAO_PWM_B_CG = 147,
	CLK_PERAO_PWM_FB1_CG = 148,
	CLK_PERAO_PWM_FB2_CG = 149,
	CLK_PERAO_PWM_FB3_CG = 150,
	CLK_PERAO_PWM_FB4_CG = 151,
	CLK_PERAO_DISP_PWM0_CG = 152,
	CLK_PERAO_DISP_PWM1_CG = 153,
	CLK_PERAO_SPI0_B_CG = 154,
	CLK_PERAO_SPI1_B_CG = 155,
	CLK_PERAO_SPI2_B_CG = 156,
	CLK_PERAO_SPI3_B_CG = 157,
	CLK_PERAO_SPI4_B_CG = 158,
	CLK_PERAO_SPI5_B_CG = 159,
	CLK_PERAO_SPI0_H_CG = 160,
	CLK_PERAO_SPI1_H_CG = 161,
	CLK_PERAO_SPI2_H_CG = 162,
	CLK_PERAO_SPI3_H_CG = 163,
	CLK_PERAO_SPI4_H_CG = 164,
	CLK_PERAO_SPI5_H_CG = 165,
	CLK_PERAO_AXI_CG = 166,
	CLK_PERAO_AHB_APB_CG = 167,
	CLK_PERAO_TL_CG = 168,
	CLK_PERAO_REF_CG = 169,
	CLK_PERAO_I2C_CG = 170,
	CLK_PERAO_DMA_B_CG = 171,
	CLK_PERAO_SSUSB0_REF_CG = 172,
	CLK_PERAO_SSUSB0_FRMCNT_CG = 173,
	CLK_PERAO_SSUSB0_SYS_CG = 174,
	CLK_PERAO_SSUSB0_XHCI_CG = 175,
	CLK_PERAO_SSUSB0_F_CG = 176,
	CLK_PERAO_SSUSB0_H_CG = 177,
	CLK_PERAO_SSUSB1_REF_CG = 178,
	CLK_PERAO_SSUSB1_FRMCNT_CG = 179,
	CLK_PERAO_SSUSB1_SYS_CG = 180,
	CLK_PERAO_SSUSB1_XHCI_CG = 181,
	CLK_PERAO_SSUSB1_F_CG = 182,
	CLK_PERAO_SSUSB1_H_CG = 183,
	CLK_PERAO_SSUSB2_REF_CG = 184,
	CLK_PERAO_SSUSB2_FRMCNT_CG = 185,
	CLK_PERAO_SSUSB2_SYS_CG = 186,
	CLK_PERAO_SSUSB2_XHCI_CG = 187,
	CLK_PERAO_SSUSB2_F_CG = 188,
	CLK_PERAO_SSUSB2_H_CG = 189,
	CLK_PERAO_SSUSB3_REF_CG = 190,
	CLK_PERAO_SSUSB3_FRMCNT_CG = 191,
	CLK_PERAO_SSUSB3_SYS_CG = 192,
	CLK_PERAO_SSUSB3_XHCI_CG = 193,
	CLK_PERAO_SSUSB3_F_CG = 194,
	CLK_PERAO_SSUSB3_H_CG = 195,
	CLK_PERAO_SSUSB4_REF_CG = 196,
	CLK_PERAO_SSUSB4_FRMCNT_CG = 197,
	CLK_PERAO_SSUSB4_SYS_CG = 198,
	CLK_PERAO_SSUSB4_XHCI_CG = 199,
	CLK_PERAO_SSUSB4_F_CG = 200,
	CLK_PERAO_SSUSB4_H_CG = 201,
	CLK_PERAO_MSDC0_CG = 202,
	CLK_PERAO_MSDC0_H_CG = 203,
	CLK_PERAO_MSDC0_FAES_CG = 204,
	CLK_PERAO_MSDC0_MST_F_CG = 205,
	CLK_PERAO_MSDC0_SLV_H_CG = 206,
	CLK_PERAO_MSDC1_CG = 207,
	CLK_PERAO_MSDC1_H_CG = 208,
	CLK_PERAO_MSDC1_MST_F_CG = 209,
	CLK_PERAO_MSDC1_SLV_H_CG = 210,
	CLK_PERAO_MSDC2_CG = 211,
	CLK_PERAO_MSDC2_H_CG = 212,
	CLK_PERAO_MSDC2_MST_F_CG = 213,
	CLK_PERAO_MSDC2_SLV_H_CG = 214,
	CLK_PERAO_SFLASH_CG = 215,
	CLK_PERAO_SFLASH_F_CG = 216,
	CLK_PERAO_SFLASH_H_CG = 217,
	CLK_PERAO_SFLASH_P_CG = 218,
	CLK_PERAO_AUDIO0_CG = 219,
	CLK_PERAO_AUDIO1_CG = 220,
	CLK_PERAO_AUDIO2_CG = 221,
	CLK_PERAO_AUXADC_26M_CG = 222,
	CLK_MDP_MUTEX0_CG = 223,
	CLK_MDP_APB_BUS_CG = 224,
	CLK_MDP_SMI0_CG = 225,
	CLK_MDP_RDMA0_CG = 226,
	CLK_MDP_RDMA2_CG = 227,
	CLK_MDP_HDR0_CG = 228,
	CLK_MDP_AAL0_CG = 229,
	CLK_MDP_RSZ0_CG = 230,
	CLK_MDP_TDSHP0_CG = 231,
	CLK_MDP_COLOR0_CG = 232,
	CLK_MDP_WROT0_CG = 233,
	CLK_MDP_FAKE_ENG0_CG = 234,
	CLK_MDPSYS_CONFIG_CG = 235,
	CLK_MDP_RDMA1_CG = 236,
	CLK_MDP_RDMA3_CG = 237,
	CLK_MDP_HDR1_CG = 238,
	CLK_MDP_AAL1_CG = 239,
	CLK_MDP_RSZ1_CG = 240,
	CLK_MDP_TDSHP1_CG = 241,
	CLK_MDP_COLOR1_CG = 242,
	CLK_MDP_WROT1_CG = 243,
	CLK_MDP_RSZ2_CG = 244,
	CLK_MDP_WROT2_CG = 245,
	CLK_MDP_RSZ3_CG = 246,
	CLK_MDP_WROT3_CG = 247,
	CLK_MDP_BIRSZ0_CG = 248,
	CLK_MDP_BIRSZ1_CG = 249,
	CLK_MFG_BG3D_CG = 250,
	CLK_SCP_SET_SPI0_CG = 251,
	CLK_SCP_SET_SPI1_CG = 252,
	CLK_SCP_IIC_I2C0_W1S_CG = 253,
	CLK_SCP_IIC_I2C1_W1S_CG = 254,
	CLK_UFSCFG_AO_REG_UNIPRO_TX_SYM_CG = 255,
	CLK_UFSCFG_AO_REG_UNIPRO_RX_SYM0_CG = 256,
	CLK_UFSCFG_AO_REG_UNIPRO_RX_SYM1_CG = 257,
	CLK_UFSCFG_AO_REG_UNIPRO_SYS_CG = 258,
	CLK_UFSCFG_AO_REG_U_SAP_CFG_CG = 259,
	CLK_UFSCFG_AO_REG_U_PHY_TOP_AHB_S_BUS_CG = 260,
	CLK_UFSCFG_REG_UFSHCI_UFS_CG = 261,
	CLK_UFSCFG_REG_UFSHCI_AES_CG = 262,
	CLK_UFSCFG_REG_UFSHCI_U_AHB_CG = 263,
	CLK_UFSCFG_REG_UFSHCI_U_AXI_CG = 264,
	CLK_VDEC_CORE_VDEC_CKEN_CG = 265,
	CLK_VDEC_CORE_VDEC_ACTIVE_CG = 266,
	CLK_VDEC_CORE_LARB_CKEN_CG = 267,
	CLK_VEN1_CKE0_LARB_CG = 268,
	CLK_VEN1_CKE1_VENC_CG = 269,
	CLK_VEN1_CKE2_JPGENC_CG = 270,
	CLK_VEN1_CKE3_JPGDEC_CG = 271,
	CLK_VEN1_CKE4_JPGDEC_C1_CG = 272,
	CLK_VEN1_CKE5_GALS_CG = 273,
	CLK_VEN1_CKE6_GALS_SRAM_CG = 274,
	TRACE_CLK_NUM = 275,
};

const char *trace_subsys_cgs[] = {
	[CLK_AFE_DL0_DAC_TML_CG] = "afe_dl0_dac_tml",
	[CLK_AFE_DL0_DAC_HIRES_CG] = "afe_dl0_dac_hires",
	[CLK_AFE_DL0_DAC_CG] = "afe_dl0_dac",
	[CLK_AFE_DL0_PREDIS_CG] = "afe_dl0_predis",
	[CLK_AFE_DL0_NLE_CG] = "afe_dl0_nle",
	[CLK_AFE_PCM0_CG] = "afe_pcm0",
	[CLK_AFE_CM1_CG] = "afe_cm1",
	[CLK_AFE_CM0_CG] = "afe_cm0",
	[CLK_AFE_HW_GAIN23_CG] = "afe_hw_gain23",
	[CLK_AFE_HW_GAIN01_CG] = "afe_hw_gain01",
	[CLK_AFE_FM_I2S_CG] = "afe_fm_i2s",
	[CLK_AFE_MTKAIFV4_CG] = "afe_mtkaifv4",
	[CLK_AFE_AUDIO_HOPPING_CG] = "afe_audio_hopping_ck",
	[CLK_AFE_AUDIO_F26M_CG] = "afe_audio_f26m_ck",
	[CLK_AFE_APLL1_CG] = "afe_apll1_ck",
	[CLK_AFE_APLL2_CG] = "afe_apll2_ck",
	[CLK_AFE_H208M_CG] = "afe_h208m_ck",
	[CLK_AFE_APLL_TUNER2_CG] = "afe_apll_tuner2",
	[CLK_AFE_APLL_TUNER1_CG] = "afe_apll_tuner1",
	[CLK_AFE_DMIC1_ADC_HIRES_TML_CG] = "afe_dmic1_aht",
	[CLK_AFE_DMIC1_ADC_HIRES_CG] = "afe_dmic1_adc_hires",
	[CLK_AFE_DMIC1_TML_CG] = "afe_dmic1_tml",
	[CLK_AFE_DMIC1_ADC_CG] = "afe_dmic1_adc",
	[CLK_AFE_DMIC0_ADC_HIRES_TML_CG] = "afe_dmic0_aht",
	[CLK_AFE_DMIC0_ADC_HIRES_CG] = "afe_dmic0_adc_hires",
	[CLK_AFE_DMIC0_TML_CG] = "afe_dmic0_tml",
	[CLK_AFE_DMIC0_ADC_CG] = "afe_dmic0_adc",
	[CLK_AFE_UL0_ADC_HIRES_TML_CG] = "afe_ul0_aht",
	[CLK_AFE_UL0_ADC_HIRES_CG] = "afe_ul0_adc_hires",
	[CLK_AFE_UL0_TML_CG] = "afe_ul0_tml",
	[CLK_AFE_UL0_ADC_CG] = "afe_ul0_adc",
	[CLK_AFE_ETDM_IN1_CG] = "afe_etdm_in1",
	[CLK_AFE_ETDM_IN0_CG] = "afe_etdm_in0",
	[CLK_AFE_ETDM_OUT4_CG] = "afe_etdm_out4",
	[CLK_AFE_ETDM_OUT1_CG] = "afe_etdm_out1",
	[CLK_AFE_ETDM_OUT0_CG] = "afe_etdm_out0",
	[CLK_AFE_TDM_OUT_CG] = "afe_tdm_out",
	[CLK_AFE_GENERAL4_ASRC_CG] = "afe_general4_asrc",
	[CLK_AFE_GENERAL3_ASRC_CG] = "afe_general3_asrc",
	[CLK_AFE_GENERAL2_ASRC_CG] = "afe_general2_asrc",
	[CLK_AFE_GENERAL1_ASRC_CG] = "afe_general1_asrc",
	[CLK_AFE_GENERAL0_ASRC_CG] = "afe_general0_asrc",
	[CLK_AFE_CONNSYS_I2S_ASRC_CG] = "afe_connsys_i2s_asrc",
	[CLK_VAD_CORE0_EN_CG] = "vad_core0",
	[CLK_VAD_BUSEMI_EN_CG] = "vad_busemi_en",
	[CLK_VAD_TIMER_EN_CG] = "vad_timer_en",
	[CLK_VAD_DMA0_EN_CG] = "vad_dma0_en",
	[CLK_VAD_UART_EN_CG] = "vad_uart_en",
	[CLK_VAD_VOWPLL_EN_CG] = "vad_vowpll_en",
	[CLK_VADSYS_26M_CG] = "vadsys_26m",
	[CLK_VADSYS_BUS_CG] = "vadsys_bus",
	[CLK_CAM_M_LARB13_CG] = "cam_m_larb13",
	[CLK_CAM_M_LARB14_CG] = "cam_m_larb14",
	[CLK_CAM_M_CAMSYS_MAIN_CAM_CG] = "cam_m_camsys_main_cam",
	[CLK_CAM_M_CAMSYS_MAIN_CAMTG_CG] = "cam_m_camsys_main_camtg",
	[CLK_CAM_M_SENINF_CG] = "cam_m_seninf",
	[CLK_CAM_M_CAMSV1_CG] = "cam_m_camsv1",
	[CLK_CAM_M_CAMSV2_CG] = "cam_m_camsv2",
	[CLK_CAM_M_CAMSV3_CG] = "cam_m_camsv3",
	[CLK_CAM_M_FAKE_ENG_CG] = "cam_m_fake_eng",
	[CLK_CAM_M_CAM2MM_GALS_CG] = "cam_m_cam2mm_gals",
	[CLK_CAM_M_CAMSV4_CG] = "cam_m_camsv4",
	[CLK_CAM_M_PDA_CG] = "cam_m_pda",
	[CLK_CAM_RA_CAMSYS_RAWA_LARBX_CG] = "cam_ra_camsys_rawa_larbx",
	[CLK_CAM_RA_CAMSYS_RAWA_CAM_CG] = "cam_ra_camsys_rawa_cam",
	[CLK_CAM_RA_CAMSYS_RAWA_CAMTG_CG] = "cam_ra_camsys_rawa_camtg",
	[CLK_CAM_RB_CAMSYS_RAWB_LARBX_CG] = "cam_rb_camsys_rawb_larbx",
	[CLK_CAM_RB_CAMSYS_RAWB_CAM_CG] = "cam_rb_camsys_rawb_cam",
	[CLK_CAM_RB_CAMSYS_RAWB_CAMTG_CG] = "cam_rb_camsys_rawb_camtg",
	[CLK_MM_DISP_OVL0_4L_CG] = "mm_disp_ovl0_4l",
	[CLK_MM_DISP_OVL1_4L_CG] = "mm_disp_ovl1_4l",
	[CLK_MM_VPP_RSZ0_CG] = "mm_vpp_rsz0",
	[CLK_MM_VPP_RSZ1_CG] = "mm_vpp_rsz1",
	[CLK_MM_DISP_RDMA0_CG] = "mm_disp_rdma0",
	[CLK_MM_DISP_RDMA1_CG] = "mm_disp_rdma1",
	[CLK_MM_DISP_COLOR0_CG] = "mm_disp_color0",
	[CLK_MM_DISP_COLOR1_CG] = "mm_disp_color1",
	[CLK_MM_DISP_CCORR0_CG] = "mm_disp_ccorr0",
	[CLK_MM_DISP_CCORR1_CG] = "mm_disp_ccorr1",
	[CLK_MM_DISP_CCORR2_CG] = "mm_disp_ccorr2",
	[CLK_MM_DISP_CCORR3_CG] = "mm_disp_ccorr3",
	[CLK_MM_DISP_AAL0_CG] = "mm_disp_aal0",
	[CLK_MM_DISP_AAL1_CG] = "mm_disp_aal1",
	[CLK_MM_DISP_GAMMA0_CG] = "mm_disp_gamma0",
	[CLK_MM_DISP_GAMMA1_CG] = "mm_disp_gamma1",
	[CLK_MM_DISP_DITHER0_CG] = "mm_disp_dither0",
	[CLK_MM_DISP_DITHER1_CG] = "mm_disp_dither1",
	[CLK_MM_DISP_DSC_WRAP0_CG] = "mm_disp_dsc_wrap0",
	[CLK_MM_VPP_MERGE0_CG] = "mm_vpp_merge0",
	[CLK_MMSYS_0_DISP_DVO_CG] = "mmsys_0_disp_dvo",
	[CLK_MMSYS_0_DISP_DSI0_CG] = "mmsys_0_CLK0",
	[CLK_MM_DP_INTF0_CG] = "mm_dp_intf0",
	[CLK_MM_DISP_WDMA0_CG] = "mm_disp_wdma0",
	[CLK_MM_DISP_WDMA1_CG] = "mm_disp_wdma1",
	[CLK_MM_DISP_FAKE_ENG0_CG] = "mm_disp_fake_eng0",
	[CLK_MM_DISP_FAKE_ENG1_CG] = "mm_disp_fake_eng1",
	[CLK_MM_SMI_LARB_CG] = "mm_smi_larb",
	[CLK_MM_DISP_MUTEX0_CG] = "mm_disp_mutex0",
	[CLK_MM_DIPSYS_CONFIG_CG] = "mm_dipsys_config",
	[CLK_MM_DUMMY_CG] = "mm_dummy",
	[CLK_MMSYS_1_DISP_DSI0_CG] = "mmsys_1_CLK0",
	[CLK_MMSYS_1_LVDS_ENCODER_CG] = "mmsys_1_lvds_encoder",
	[CLK_MMSYS_1_DISP_DVO_CG] = "mmsys_1_disp_dvo",
	[CLK_MM_DP_INTF_CG] = "mm_dp_intf",
	[CLK_MMSYS_1_LVDS_ENCODER_CTS_CG] = "mmsys_1_lvds_encoder_cts",
	[CLK_MMSYS_1_DISP_DVO_AVT_CG] = "mmsys_1_disp_dvo_avt",
	[CLK_GCE_D_TOP_CG] = "gce_d_top",
	[CLK_GCE_M_TOP_CG] = "gce_m_top",
	[CLK_MMINFRA_GCE_D_CG] = "mminfra_gce_d",
	[CLK_MMINFRA_GCE_M_CG] = "mminfra_gce_m",
	[CLK_MMINFRA_SMI_CG] = "mminfra_smi",
	[CLK_MMINFRA_GCE_26M_CG] = "mminfra_gce_26m",
	[CLK_IMGSYS1_LARB9_CG] = "imgsys1_larb9",
	[CLK_IMGSYS1_LARB11_CG] = "imgsys1_larb11",
	[CLK_IMGSYS1_DIP_CG] = "imgsys1_dip",
	[CLK_IMGSYS1_GALS_CG] = "imgsys1_gals",
	[CLK_IMGSYS2_LARB9_CG] = "imgsys2_larb9",
	[CLK_IMGSYS2_LARB11_CG] = "imgsys2_larb11",
	[CLK_IMGSYS2_MFB_CG] = "imgsys2_mfb",
	[CLK_IMGSYS2_WPE_CG] = "imgsys2_wpe",
	[CLK_IMGSYS2_MSS_CG] = "imgsys2_mss",
	[CLK_IMGSYS2_GALS_CG] = "imgsys2_gals",
	[CLK_IPE_LARB19_CG] = "ipe_larb19",
	[CLK_IPE_LARB20_CG] = "ipe_larb20",
	[CLK_IPE_SMI_SUBCOM_CG] = "ipe_smi_subcom",
	[CLK_IPE_FD_CG] = "ipe_fd",
	[CLK_IPE_FE_CG] = "ipe_fe",
	[CLK_IPE_RSC_CG] = "ipe_rsc",
	[CLK_IPESYS_GALS_CG] = "ipesys_gals",
	[CLK_IMPE_I2C0_CG] = "impe_i2c0",
	[CLK_IMPE_I2C1_CG] = "impe_i2c1",
	[CLK_IMPEN_I2C7_CG] = "impen_i2c7",
	[CLK_IMPEN_I2C8_CG] = "impen_i2c8",
	[CLK_IMPS_I2C3_CG] = "imps_i2c3",
	[CLK_IMPS_I2C4_CG] = "imps_i2c4",
	[CLK_IMPS_I2C5_CG] = "imps_i2c5",
	[CLK_IMPS_I2C6_CG] = "imps_i2c6",
	[CLK_IMPWS_I2C2_CG] = "impws_i2c2",
	[CLK_IFRAO_CQ_DMA_FPC_CG] = "ifrao_dma",
	[CLK_IFRAO_DEBUGSYS_CG] = "ifrao_debugsys",
	[CLK_IFRAO_DBG_TRACE_CG] = "ifrao_dbg_trace",
	[CLK_IFRAO_CQ_DMA_CG] = "ifrao_cq_dma",
	[CLK_PERAO_UART0_CG] = "perao_uart0",
	[CLK_PERAO_UART1_CG] = "perao_uart1",
	[CLK_PERAO_UART2_CG] = "perao_uart2",
	[CLK_PERAO_UART3_CG] = "perao_uart3",
	[CLK_PERAO_PWM_H_CG] = "perao_pwm_h",
	[CLK_PERAO_PWM_B_CG] = "perao_pwm_b",
	[CLK_PERAO_PWM_FB1_CG] = "perao_pwm_fb1",
	[CLK_PERAO_PWM_FB2_CG] = "perao_pwm_fb2",
	[CLK_PERAO_PWM_FB3_CG] = "perao_pwm_fb3",
	[CLK_PERAO_PWM_FB4_CG] = "perao_pwm_fb4",
	[CLK_PERAO_DISP_PWM0_CG] = "perao_disp_pwm0",
	[CLK_PERAO_DISP_PWM1_CG] = "perao_disp_pwm1",
	[CLK_PERAO_SPI0_B_CG] = "perao_spi0_b",
	[CLK_PERAO_SPI1_B_CG] = "perao_spi1_b",
	[CLK_PERAO_SPI2_B_CG] = "perao_spi2_b",
	[CLK_PERAO_SPI3_B_CG] = "perao_spi3_b",
	[CLK_PERAO_SPI4_B_CG] = "perao_spi4_b",
	[CLK_PERAO_SPI5_B_CG] = "perao_spi5_b",
	[CLK_PERAO_SPI0_H_CG] = "perao_spi0_h",
	[CLK_PERAO_SPI1_H_CG] = "perao_spi1_h",
	[CLK_PERAO_SPI2_H_CG] = "perao_spi2_h",
	[CLK_PERAO_SPI3_H_CG] = "perao_spi3_h",
	[CLK_PERAO_SPI4_H_CG] = "perao_spi4_h",
	[CLK_PERAO_SPI5_H_CG] = "perao_spi5_h",
	[CLK_PERAO_AXI_CG] = "perao_axi",
	[CLK_PERAO_AHB_APB_CG] = "perao_ahb_apb",
	[CLK_PERAO_TL_CG] = "perao_tl",
	[CLK_PERAO_REF_CG] = "perao_ref",
	[CLK_PERAO_I2C_CG] = "perao_i2c",
	[CLK_PERAO_DMA_B_CG] = "perao_dma_b",
	[CLK_PERAO_SSUSB0_REF_CG] = "perao_ssusb0_ref",
	[CLK_PERAO_SSUSB0_FRMCNT_CG] = "perao_ssusb0_frmcnt",
	[CLK_PERAO_SSUSB0_SYS_CG] = "perao_ssusb0_sys",
	[CLK_PERAO_SSUSB0_XHCI_CG] = "perao_ssusb0_xhci",
	[CLK_PERAO_SSUSB0_F_CG] = "perao_ssusb0_f",
	[CLK_PERAO_SSUSB0_H_CG] = "perao_ssusb0_h",
	[CLK_PERAO_SSUSB1_REF_CG] = "perao_ssusb1_ref",
	[CLK_PERAO_SSUSB1_FRMCNT_CG] = "perao_ssusb1_frmcnt",
	[CLK_PERAO_SSUSB1_SYS_CG] = "perao_ssusb1_sys",
	[CLK_PERAO_SSUSB1_XHCI_CG] = "perao_ssusb1_xhci",
	[CLK_PERAO_SSUSB1_F_CG] = "perao_ssusb1_f",
	[CLK_PERAO_SSUSB1_H_CG] = "perao_ssusb1_h",
	[CLK_PERAO_SSUSB2_REF_CG] = "perao_ssusb2_ref",
	[CLK_PERAO_SSUSB2_FRMCNT_CG] = "perao_ssusb2_frmcnt",
	[CLK_PERAO_SSUSB2_SYS_CG] = "perao_ssusb2_sys",
	[CLK_PERAO_SSUSB2_XHCI_CG] = "perao_ssusb2_xhci",
	[CLK_PERAO_SSUSB2_F_CG] = "perao_ssusb2_f",
	[CLK_PERAO_SSUSB2_H_CG] = "perao_ssusb2_h",
	[CLK_PERAO_SSUSB3_REF_CG] = "perao_ssusb3_ref",
	[CLK_PERAO_SSUSB3_FRMCNT_CG] = "perao_ssusb3_frmcnt",
	[CLK_PERAO_SSUSB3_SYS_CG] = "perao_ssusb3_sys",
	[CLK_PERAO_SSUSB3_XHCI_CG] = "perao_ssusb3_xhci",
	[CLK_PERAO_SSUSB3_F_CG] = "perao_ssusb3_f",
	[CLK_PERAO_SSUSB3_H_CG] = "perao_ssusb3_h",
	[CLK_PERAO_SSUSB4_REF_CG] = "perao_ssusb4_ref",
	[CLK_PERAO_SSUSB4_FRMCNT_CG] = "perao_ssusb4_frmcnt",
	[CLK_PERAO_SSUSB4_SYS_CG] = "perao_ssusb4_sys",
	[CLK_PERAO_SSUSB4_XHCI_CG] = "perao_ssusb4_xhci",
	[CLK_PERAO_SSUSB4_F_CG] = "perao_ssusb4_f",
	[CLK_PERAO_SSUSB4_H_CG] = "perao_ssusb4_h",
	[CLK_PERAO_MSDC0_CG] = "perao_msdc0",
	[CLK_PERAO_MSDC0_H_CG] = "perao_msdc0_h",
	[CLK_PERAO_MSDC0_FAES_CG] = "perao_msdc0_faes",
	[CLK_PERAO_MSDC0_MST_F_CG] = "perao_msdc0_mst_f",
	[CLK_PERAO_MSDC0_SLV_H_CG] = "perao_msdc0_slv_h",
	[CLK_PERAO_MSDC1_CG] = "perao_msdc1",
	[CLK_PERAO_MSDC1_H_CG] = "perao_msdc1_h",
	[CLK_PERAO_MSDC1_MST_F_CG] = "perao_msdc1_mst_f",
	[CLK_PERAO_MSDC1_SLV_H_CG] = "perao_msdc1_slv_h",
	[CLK_PERAO_MSDC2_CG] = "perao_msdc2",
	[CLK_PERAO_MSDC2_H_CG] = "perao_msdc2_h",
	[CLK_PERAO_MSDC2_MST_F_CG] = "perao_msdc2_mst_f",
	[CLK_PERAO_MSDC2_SLV_H_CG] = "perao_msdc2_slv_h",
	[CLK_PERAO_SFLASH_CG] = "perao_sflash",
	[CLK_PERAO_SFLASH_F_CG] = "perao_sflash_f",
	[CLK_PERAO_SFLASH_H_CG] = "perao_sflash_h",
	[CLK_PERAO_SFLASH_P_CG] = "perao_sflash_p",
	[CLK_PERAO_AUDIO0_CG] = "perao_audio0",
	[CLK_PERAO_AUDIO1_CG] = "perao_audio1",
	[CLK_PERAO_AUDIO2_CG] = "perao_audio2",
	[CLK_PERAO_AUXADC_26M_CG] = "perao_auxadc_26m",
	[CLK_MDP_MUTEX0_CG] = "mdp_mutex0",
	[CLK_MDP_APB_BUS_CG] = "mdp_apb_bus",
	[CLK_MDP_SMI0_CG] = "mdp_smi0",
	[CLK_MDP_RDMA0_CG] = "mdp_rdma0",
	[CLK_MDP_RDMA2_CG] = "mdp_rdma2",
	[CLK_MDP_HDR0_CG] = "mdp_hdr0",
	[CLK_MDP_AAL0_CG] = "mdp_aal0",
	[CLK_MDP_RSZ0_CG] = "mdp_rsz0",
	[CLK_MDP_TDSHP0_CG] = "mdp_tdshp0",
	[CLK_MDP_COLOR0_CG] = "mdp_color0",
	[CLK_MDP_WROT0_CG] = "mdp_wrot0",
	[CLK_MDP_FAKE_ENG0_CG] = "mdp_fake_eng0",
	[CLK_MDPSYS_CONFIG_CG] = "mdpsys_config",
	[CLK_MDP_RDMA1_CG] = "mdp_rdma1",
	[CLK_MDP_RDMA3_CG] = "mdp_rdma3",
	[CLK_MDP_HDR1_CG] = "mdp_hdr1",
	[CLK_MDP_AAL1_CG] = "mdp_aal1",
	[CLK_MDP_RSZ1_CG] = "mdp_rsz1",
	[CLK_MDP_TDSHP1_CG] = "mdp_tdshp1",
	[CLK_MDP_COLOR1_CG] = "mdp_color1",
	[CLK_MDP_WROT1_CG] = "mdp_wrot1",
	[CLK_MDP_RSZ2_CG] = "mdp_rsz2",
	[CLK_MDP_WROT2_CG] = "mdp_wrot2",
	[CLK_MDP_RSZ3_CG] = "mdp_rsz3",
	[CLK_MDP_WROT3_CG] = "mdp_wrot3",
	[CLK_MDP_BIRSZ0_CG] = "mdp_birsz0",
	[CLK_MDP_BIRSZ1_CG] = "mdp_birsz1",
	[CLK_MFG_BG3D_CG] = "mfg_bg3d",
	[CLK_SCP_SET_SPI0_CG] = "scp_set_spi0",
	[CLK_SCP_SET_SPI1_CG] = "scp_set_spi1",
	[CLK_SCP_IIC_I2C0_W1S_CG] = "scp_iic_i2c0_w1s",
	[CLK_SCP_IIC_I2C1_W1S_CG] = "scp_iic_i2c1_w1s",
	[CLK_UFSCFG_AO_REG_UNIPRO_TX_SYM_CG] = "ufscfg_ao_unipro_tx_sym",
	[CLK_UFSCFG_AO_REG_UNIPRO_RX_SYM0_CG] = "ufscfg_ao_unipro_rx_sym0",
	[CLK_UFSCFG_AO_REG_UNIPRO_RX_SYM1_CG] = "ufscfg_ao_unipro_rx_sym1",
	[CLK_UFSCFG_AO_REG_UNIPRO_SYS_CG] = "ufscfg_ao_unipro_sys",
	[CLK_UFSCFG_AO_REG_U_SAP_CFG_CG] = "ufscfg_ao_u_sap_cfg",
	[CLK_UFSCFG_AO_REG_U_PHY_TOP_AHB_S_BUS_CG] = "ufscfg_ao_u_phy_ahb_s_bus",
	[CLK_UFSCFG_REG_UFSHCI_UFS_CG] = "ufscfg_ufshci_ufs",
	[CLK_UFSCFG_REG_UFSHCI_AES_CG] = "ufscfg_ufshci_aes",
	[CLK_UFSCFG_REG_UFSHCI_U_AHB_CG] = "ufscfg_ufshci_u_ahb",
	[CLK_UFSCFG_REG_UFSHCI_U_AXI_CG] = "ufscfg_ufshci_u_axi",
	[CLK_VDEC_CORE_VDEC_CKEN_CG] = "vdec_core_vdec_cken",
	[CLK_VDEC_CORE_VDEC_ACTIVE_CG] = "vdec_core_vdec_active",
	[CLK_VDEC_CORE_LARB_CKEN_CG] = "vdec_core_larb_cken",
	[CLK_VEN1_CKE0_LARB_CG] = "ven1_larb",
	[CLK_VEN1_CKE1_VENC_CG] = "ven1_venc",
	[CLK_VEN1_CKE2_JPGENC_CG] = "ven1_jpgenc",
	[CLK_VEN1_CKE3_JPGDEC_CG] = "ven1_jpgdec",
	[CLK_VEN1_CKE4_JPGDEC_C1_CG] = "ven1_jpgdec_c1",
	[CLK_VEN1_CKE5_GALS_CG] = "ven1_gals",
	[CLK_VEN1_CKE6_GALS_SRAM_CG] = "ven1_gals_sram",
	[TRACE_CLK_NUM] = "NULL",
};

struct clkchk_fm {
	const char *fm_name;
	unsigned int fm_id;
	unsigned int fm_type;
};

/* check which fmeter clk you want to get freq */
enum {
	CHK_FM_MMPLL2 = 0,
	CHK_FM_NUM,
};

/* fill in the fmeter clk you want to get freq */
struct  clkchk_fm chk_fm_list[] = {
	{},
};

static void trace_clk_event(const char *name, unsigned int clk_sta)
{
	unsigned long flags = 0;
	int i;

	spin_lock_irqsave(&clk_trace_lock, flags);

	if (!name)
		goto OUT;

	for (i = 0; i < TRACE_CLK_NUM; i++) {
		if (!strcmp(trace_subsys_cgs[i], name))
			break;
	}

	if (i == TRACE_CLK_NUM)
		goto OUT;

	clk_event[evt_cnt] = (i << CLK_ID_SHIFT) | (clk_sta << CLK_STA_SHIFT);
	evt_cnt++;
	if (evt_cnt >= EVT_LEN)
		evt_cnt = 0;

OUT:
	spin_unlock_irqrestore(&clk_trace_lock, flags);
}

/*
 * clkchk dump_regs
 */

#define REGBASE_V(_phys, _id_name, _pg, _pn) { .phys = _phys, .id = _id_name,	\
		.name = #_id_name, .pg = _pg, .pn = _pn}

static struct regbase rb[] = {
	[top] = REGBASE_V(0x10000000, top, PD_NULL, CLK_NULL),
	[ifrao] = REGBASE_V(0x10001000, ifrao, PD_NULL, CLK_NULL),
	[infracfg_ao_reg_bus] = REGBASE_V(0x10001000, infracfg_ao_reg_bus, PD_NULL, CLK_NULL),
	[apmixed] = REGBASE_V(0x1000C000, apmixed, PD_NULL, CLK_NULL),
	[emicfg_ao_mem] = REGBASE_V(0x10270000, emicfg_ao_mem, PD_NULL, CLK_NULL),
	[perao] = REGBASE_V(0x11036000, perao, PD_NULL, CLK_NULL),
	[afe] = REGBASE_V(0x11050000, afe, MT8189_CHK_PD_AUDIO, CLK_NULL),
	[ufscfg_ao_reg] = REGBASE_V(0x112b8000, ufscfg_ao_reg, PD_NULL, CLK_NULL),
	[ufscfg_pdn_reg] = REGBASE_V(0x112bb000, ufscfg_pdn_reg, MT8189_CHK_PD_UFS0, CLK_NULL),
	[impws] = REGBASE_V(0x11B21000, impws, PD_NULL, CLK_NULL),
	[impe] = REGBASE_V(0x11C22000, impe, PD_NULL, CLK_NULL),
	[imps] = REGBASE_V(0x11D74000, imps, PD_NULL, CLK_NULL),
	[impen] = REGBASE_V(0x11F32000, impen, PD_NULL, CLK_NULL),
	[mfg] = REGBASE_V(0x13FBF000, mfg, MT8189_CHK_PD_MFG0, CLK_NULL),
	[mm] = REGBASE_V(0x14000000, mm, MT8189_CHK_PD_DIS0, CLK_NULL),
	[imgsys1] = REGBASE_V(0x15020000, imgsys1, MT8189_CHK_PD_ISP_IMG1, CLK_NULL),
	[imgsys2] = REGBASE_V(0x15820000, imgsys2, MT8189_CHK_PD_ISP_IMG2, CLK_NULL),
	[vdec_core] = REGBASE_V(0x1602f000, vdec_core, MT8189_CHK_PD_VDE0, CLK_NULL),
	[ven1] = REGBASE_V(0x17000000, ven1, MT8189_CHK_PD_VEN0, CLK_NULL),
	[spm] = REGBASE_V(0x1C001000, spm, PD_NULL, CLK_NULL),
	[vlpcfg_reg_bus] = REGBASE_V(0x1C00C000, vlpcfg_reg_bus, PD_NULL, CLK_NULL),
	[vlp_ck] = REGBASE_V(0x1C012000, vlp_ck, PD_NULL, CLK_NULL),
	[scp_iic] = REGBASE_V(0x1C80A000, scp_iic, PD_NULL, CLK_NULL),
	[scp] = REGBASE_V(0x1CB21000, scp, PD_NULL, CLK_NULL),
	[vad] = REGBASE_V(0x1E010000, vad, MT8189_CHK_PD_ADSP_AO, CLK_NULL),
	[cam_m] = REGBASE_V(0x1a000000, cam_m, MT8189_CHK_PD_CAM_MAIN, CLK_NULL),
	[cam_ra] = REGBASE_V(0x1a04f000, cam_ra, MT8189_CHK_PD_CAM_SUBA, CLK_NULL),
	[cam_rb] = REGBASE_V(0x1a06f000, cam_rb, MT8189_CHK_PD_CAM_SUBB, CLK_NULL),
	[ipe] = REGBASE_V(0x1b000000, ipe, MT8189_CHK_PD_ISP_IPE, CLK_NULL),
	[vlpcfg_ao_reg] = REGBASE_V(0x1c000000, vlpcfg_ao_reg, PD_NULL, CLK_NULL),
	[dvfsrc_top] = REGBASE_V(0x1c00f000, dvfsrc_top, PD_NULL, CLK_NULL),
	[mminfra_config] = REGBASE_V(0x1e800000, mminfra_config, MT8189_CHK_PD_MM_INFRA, CLK_NULL),
	[gce_d] = REGBASE_V(0x1e980000, gce_d, MT8189_CHK_PD_MDP0, "mminfra_gce_d"),
	[gce_m] = REGBASE_V(0x1e990000, gce_m, MT8189_CHK_PD_MDP0, "mminfra_gce_m"),
	[mdp] = REGBASE_V(0x1f000000, mdp, MT8189_CHK_PD_MDP0, CLK_NULL),
	[dbgao] = REGBASE_V(0xD01A000, dbgao, PD_NULL, CLK_NULL),
	[dem] = REGBASE_V(0xd0a0000, dem, PD_NULL, CLK_NULL),
	{},
};

#define REGNAME(_base, _ofs, _name)	\
	{ .base = &rb[_base], .id = _base, .ofs = _ofs, .name = #_name }

static struct regname rn[] = {
	/* TOPCKGEN register */
	REGNAME(top, 0x0010, CLK_CFG_0),
	REGNAME(top, 0x0020, CLK_CFG_1),
	REGNAME(top, 0x0030, CLK_CFG_2),
	REGNAME(top, 0x0040, CLK_CFG_3),
	REGNAME(top, 0x0050, CLK_CFG_4),
	REGNAME(top, 0x0060, CLK_CFG_5),
	REGNAME(top, 0x0070, CLK_CFG_6),
	REGNAME(top, 0x0080, CLK_CFG_7),
	REGNAME(top, 0x0090, CLK_CFG_8),
	REGNAME(top, 0x00A0, CLK_CFG_9),
	REGNAME(top, 0x00B0, CLK_CFG_10),
	REGNAME(top, 0x00C0, CLK_CFG_11),
	REGNAME(top, 0x00D0, CLK_CFG_12),
	REGNAME(top, 0x00E0, CLK_CFG_13),
	REGNAME(top, 0x00F0, CLK_CFG_14),
	REGNAME(top, 0x0100, CLK_CFG_15),
	REGNAME(top, 0x0110, CLK_CFG_16),
	REGNAME(top, 0x0180, CLK_CFG_17),
	REGNAME(top, 0x0190, CLK_CFG_18),
	REGNAME(top, 0x0240, CLK_CFG_19),
	REGNAME(top, 0x0320, CLK_AUDDIV_0),
	REGNAME(top, 0x0510, CLK_MISC_CFG_3),
	REGNAME(top, 0x0328, CLK_AUDDIV_2),
	REGNAME(top, 0x0334, CLK_AUDDIV_3),
	REGNAME(top, 0x033C, CLK_AUDDIV_5),
	REGNAME(top, 0x510, CLK_MISC_CFG_3),
	/* INFRACFG_AO register */
	REGNAME(ifrao, 0x90, MODULE_CG_0),
	REGNAME(ifrao, 0x94, MODULE_CG_1),
	REGNAME(ifrao, 0xAC, MODULE_CG_2),
	REGNAME(ifrao, 0xC8, MODULE_CG_3),
	/* INFRA_INFRACFG_AO_REG_BUS register */
	REGNAME(infracfg_ao_reg_bus, 0x0C90, MCU_CONNSYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C9C, MCU_CONNSYS_PROTECT_RDY_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C50, INFRASYS_PROTECT_EN_STA_1),
	REGNAME(infracfg_ao_reg_bus, 0x0C5C, INFRASYS_PROTECT_RDY_STA_1),
	REGNAME(infracfg_ao_reg_bus, 0x0C40, INFRASYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C4C, INFRASYS_PROTECT_RDY_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C80, PERISYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C8C, PERISYS_PROTECT_RDY_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C10, MMSYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C1C, MMSYS_PROTECT_RDY_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C20, MMSYS_PROTECT_EN_STA_1),
	REGNAME(infracfg_ao_reg_bus, 0x0C2C, MMSYS_PROTECT_RDY_STA_1),
	REGNAME(infracfg_ao_reg_bus, 0x0C60, EMISYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0C6C, EMISYS_PROTECT_RDY_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0CA0, MD_MFGSYS_PROTECT_EN_STA_0),
	REGNAME(infracfg_ao_reg_bus, 0x0CAC, MD_MFGSYS_PROTECT_RDY_STA_0),
	/* APMIXEDSYS register */
	REGNAME(apmixed, 0x204, ARMPLL_LL_CON0),
	REGNAME(apmixed, 0x208, ARMPLL_LL_CON1),
	REGNAME(apmixed, 0x20c, ARMPLL_LL_CON2),
	REGNAME(apmixed, 0x210, ARMPLL_LL_CON3),
	REGNAME(apmixed, 0x214, ARMPLL_BL_CON0),
	REGNAME(apmixed, 0x218, ARMPLL_BL_CON1),
	REGNAME(apmixed, 0x21c, ARMPLL_BL_CON2),
	REGNAME(apmixed, 0x220, ARMPLL_BL_CON3),
	REGNAME(apmixed, 0x224, CCIPLL_CON0),
	REGNAME(apmixed, 0x228, CCIPLL_CON1),
	REGNAME(apmixed, 0x22c, CCIPLL_CON2),
	REGNAME(apmixed, 0x230, CCIPLL_CON3),
	REGNAME(apmixed, 0x304, MAINPLL_CON0),
	REGNAME(apmixed, 0x308, MAINPLL_CON1),
	REGNAME(apmixed, 0x30c, MAINPLL_CON2),
	REGNAME(apmixed, 0x310, MAINPLL_CON3),
	REGNAME(apmixed, 0x314, UNIVPLL_CON0),
	REGNAME(apmixed, 0x318, UNIVPLL_CON1),
	REGNAME(apmixed, 0x31c, UNIVPLL_CON2),
	REGNAME(apmixed, 0x320, UNIVPLL_CON3),
	REGNAME(apmixed, 0x324, MMPLL_CON0),
	REGNAME(apmixed, 0x328, MMPLL_CON1),
	REGNAME(apmixed, 0x32c, MMPLL_CON2),
	REGNAME(apmixed, 0x330, MMPLL_CON3),
	REGNAME(apmixed, 0x504, MFGPLL_CON0),
	REGNAME(apmixed, 0x508, MFGPLL_CON1),
	REGNAME(apmixed, 0x50c, MFGPLL_CON2),
	REGNAME(apmixed, 0x510, MFGPLL_CON3),
	REGNAME(apmixed, 0x404, APLL1_CON0),
	REGNAME(apmixed, 0x408, APLL1_CON1),
	REGNAME(apmixed, 0x40c, APLL1_CON2),
	REGNAME(apmixed, 0x410, APLL1_CON3),
	REGNAME(apmixed, 0x414, APLL1_CON4),
	REGNAME(apmixed, 0x0040, APLL1_TUNER_CON0),
	REGNAME(apmixed, 0x000C, AP_PLL_CON3),
	REGNAME(apmixed, 0x418, APLL2_CON0),
	REGNAME(apmixed, 0x41c, APLL2_CON1),
	REGNAME(apmixed, 0x420, APLL2_CON2),
	REGNAME(apmixed, 0x424, APLL2_CON3),
	REGNAME(apmixed, 0x428, APLL2_CON4),
	REGNAME(apmixed, 0x0044, APLL2_TUNER_CON0),
	REGNAME(apmixed, 0x000C, AP_PLL_CON3),
	REGNAME(apmixed, 0x334, EMIPLL_CON0),
	REGNAME(apmixed, 0x338, EMIPLL_CON1),
	REGNAME(apmixed, 0x33c, EMIPLL_CON2),
	REGNAME(apmixed, 0x340, EMIPLL_CON3),
	REGNAME(apmixed, 0x614, APUPLL2_CON0),
	REGNAME(apmixed, 0x618, APUPLL2_CON1),
	REGNAME(apmixed, 0x61c, APUPLL2_CON2),
	REGNAME(apmixed, 0x620, APUPLL2_CON3),
	REGNAME(apmixed, 0x604, APUPLL_CON0),
	REGNAME(apmixed, 0x608, APUPLL_CON1),
	REGNAME(apmixed, 0x60c, APUPLL_CON2),
	REGNAME(apmixed, 0x610, APUPLL_CON3),
	REGNAME(apmixed, 0x42c, TVDPLL1_CON0),
	REGNAME(apmixed, 0x430, TVDPLL1_CON1),
	REGNAME(apmixed, 0x434, TVDPLL1_CON2),
	REGNAME(apmixed, 0x438, TVDPLL1_CON3),
	REGNAME(apmixed, 0x43c, TVDPLL2_CON0),
	REGNAME(apmixed, 0x440, TVDPLL2_CON1),
	REGNAME(apmixed, 0x444, TVDPLL2_CON2),
	REGNAME(apmixed, 0x448, TVDPLL2_CON3),
	REGNAME(apmixed, 0x514, ETHPLL_CON0),
	REGNAME(apmixed, 0x518, ETHPLL_CON1),
	REGNAME(apmixed, 0x51c, ETHPLL_CON2),
	REGNAME(apmixed, 0x520, ETHPLL_CON3),
	REGNAME(apmixed, 0x524, MSDCPLL_CON0),
	REGNAME(apmixed, 0x528, MSDCPLL_CON1),
	REGNAME(apmixed, 0x52c, MSDCPLL_CON2),
	REGNAME(apmixed, 0x530, MSDCPLL_CON3),
	REGNAME(apmixed, 0x534, UFSPLL_CON0),
	REGNAME(apmixed, 0x538, UFSPLL_CON1),
	REGNAME(apmixed, 0x53c, UFSPLL_CON2),
	REGNAME(apmixed, 0x540, UFSPLL_CON3),
	/* EMICFG_AO_MEM register */
	REGNAME(emicfg_ao_mem, 0x0080, GALS_SLP_PROT_EN),
	REGNAME(emicfg_ao_mem, 0x008C, GALS_SLP_PROT_RDY),
	/* PERICFG_AO register */
	REGNAME(perao, 0x10, PERI_CG_0),
	REGNAME(perao, 0x14, PERI_CG_1),
	REGNAME(perao, 0x18, PERI_CG_2),
	/* AFE register */
	REGNAME(afe, 0x0, AUDIO_TOP_0),
	REGNAME(afe, 0x4, AUDIO_TOP_1),
	REGNAME(afe, 0x8, AUDIO_TOP_2),
	REGNAME(afe, 0xC, AUDIO_TOP_3),
	REGNAME(afe, 0x10, AUDIO_TOP_4),
	/* UFSCFG_AO_REG register */
	REGNAME(ufscfg_ao_reg, 0x4, UFS_AO_CG_0),
	/* UFSCFG_PDN_REG register */
	REGNAME(ufscfg_pdn_reg, 0x4, UFS_PDN_CG_0),
	/* IMP_IIC_WRAP_WS register */
	REGNAME(impws, 0xE00, AP_CLOCK_CG),
	/* IMP_IIC_WRAP_E register */
	REGNAME(impe, 0xE00, AP_CLOCK_CG),
	/* IMP_IIC_WRAP_S register */
	REGNAME(imps, 0xE00, AP_CLOCK_CG),
	/* IMP_IIC_WRAP_EN register */
	REGNAME(impen, 0xE00, AP_CLOCK_CG),
	/* MFG register */
	REGNAME(mfg, 0x0, MFG_CG_CON),
	/* DISPSYS_CONFIG register */
	REGNAME(mm, 0x100, MMSYS_CG_0),
	REGNAME(mm, 0x110, MMSYS_CG_1),
	/* IMGSYS1 register */
	REGNAME(imgsys1, 0x0, IMG_CG),
	/* IMGSYS2 register */
	REGNAME(imgsys2, 0x0, IMG_CG),
	/* VDEC_CORE register */
	REGNAME(vdec_core, 0x8, LARB_CKEN_CON),
	REGNAME(vdec_core, 0x0, VDEC_CKEN),
	/* VENC_GCON register */
	REGNAME(ven1, 0x0, VENCSYS_CG),
	/* SPM register */
	REGNAME(spm, 0xE04, CONN_PWR_CON),
	REGNAME(spm, 0xF40, PWR_STATUS),
	REGNAME(spm, 0xF44, PWR_STATUS_2ND),
	REGNAME(spm, 0xE10, UFS0_PWR_CON),
	REGNAME(spm, 0xE14, UFS0_PHY_PWR_CON),
	REGNAME(spm, 0xE18, AUDIO_PWR_CON),
	REGNAME(spm, 0xE1C, ADSP_TOP_PWR_CON),
	REGNAME(spm, 0xE20, ADSP_INFRA_PWR_CON),
	REGNAME(spm, 0xE24, ADSP_AO_PWR_CON),
	REGNAME(spm, 0xE28, ISP_IMG1_PWR_CON),
	REGNAME(spm, 0xE2C, ISP_IMG2_PWR_CON),
	REGNAME(spm, 0xE30, ISP_IPE_PWR_CON),
	REGNAME(spm, 0xE38, VDE0_PWR_CON),
	REGNAME(spm, 0xE40, VEN0_PWR_CON),
	REGNAME(spm, 0xE48, CAM_MAIN_PWR_CON),
	REGNAME(spm, 0xE50, CAM_SUBA_PWR_CON),
	REGNAME(spm, 0xE54, CAM_SUBB_PWR_CON),
	REGNAME(spm, 0xE68, MDP0_PWR_CON),
	REGNAME(spm, 0xE70, DIS0_PWR_CON),
	REGNAME(spm, 0xE78, MM_INFRA_PWR_CON),
	REGNAME(spm, 0xE80, DP_TX_PWR_CON),
	REGNAME(spm, 0xE84, SCP_CORE_PWR_CON),
	REGNAME(spm, 0xE88, SCP_PERI_PWR_CON),
	REGNAME(spm, 0xE9C, CSI_RX_PWR_CON),
	REGNAME(spm, 0xEA8, SSUSB_PWR_CON),
	REGNAME(spm, 0xEB4, MFG0_PWR_CON),
	REGNAME(spm, 0xEB8, MFG1_PWR_CON),
	REGNAME(spm, 0xEBC, MFG2_PWR_CON),
	REGNAME(spm, 0xEC0, MFG3_PWR_CON),
	REGNAME(spm, 0xF70, EDP_TX_PWR_CON),
	REGNAME(spm, 0xF74, PCIE_PWR_CON),
	REGNAME(spm, 0xF78, PCIE_PHY_PWR_CON),
	/* VLPCFG_REG_BUS register */
	REGNAME(vlpcfg_reg_bus, 0x0210, VLP_TOPAXI_PROTECTEN),
	REGNAME(vlpcfg_reg_bus, 0x0220, VLP_TOPAXI_PROTECTEN_STA1),
	REGNAME(vlpcfg_reg_bus, 0x091C, VLPCFG_RSVD7_ADDR),
	REGNAME(vlpcfg_reg_bus, 0x091C, VLPCFG_RSVD7_ADDR),
	/* VLP_CKSYS register */
	REGNAME(vlp_ck, 0x0008, VLP_CLK_CFG_0),
	REGNAME(vlp_ck, 0x0014, VLP_CLK_CFG_1),
	REGNAME(vlp_ck, 0x0020, VLP_CLK_CFG_2),
	REGNAME(vlp_ck, 0x002C, VLP_CLK_CFG_3),
	REGNAME(vlp_ck, 0x0038, VLP_CLK_CFG_4),
	REGNAME(vlp_ck, 0x0044, VLP_CLK_CFG_5),
	REGNAME(vlp_ck, 0x1F0, VLP_CLK_CFG_30),
	/* SCP_IIC register */
	REGNAME(scp_iic, 0xE10, CCU_CLOCK_CG),
	/* SCP register */
	REGNAME(scp, 0x154, AP_SPI_CG),
	/* VADSYS register */
	REGNAME(vad, 0x0, VADSYS_CK_EN),
	REGNAME(vad, 0x180, VOW_AUDIODSP_SW_CG),
	/* CAMSYS_MAIN register */
	REGNAME(cam_m, 0x0, CAMSYS_CG),
	/* CAMSYS_RAWA register */
	REGNAME(cam_ra, 0x0, CAMSYS_CG),
	/* CAMSYS_RAWB register */
	REGNAME(cam_rb, 0x0, CAMSYS_CG),
	/* IPESYS register */
	REGNAME(ipe, 0x0, IMG_CG),
	/* VLPCFG_AO_REG register */
	REGNAME(vlpcfg_ao_reg, 0x800, DEBUGTOP_VLPAO_CTRL),
	/* DVFSRC_TOP register */
	REGNAME(dvfsrc_top, 0x0, DVFSRC_BASIC_CONTROL),
	/* MMINFRA_CONFIG register */
	REGNAME(mminfra_config, 0x100, MMINFRA_CG_0),
	REGNAME(mminfra_config, 0x110, MMINFRA_CG_1),
	/* GCE_D register */
	REGNAME(gce_d, 0xF0, GCE_CTL_INT0),
	/* GCE_M register */
	REGNAME(gce_m, 0xF0, GCE_CTL_INT0),
	/* MDPSYS_CONFIG register */
	REGNAME(mdp, 0x100, MDPSYS_CG_0),
	REGNAME(mdp, 0x110, MDPSYS_CG_1),
	/* DBGAO register */
	REGNAME(dbgao, 0x70, ATB),
	/* DEM register */
	REGNAME(dem, 0x70, ATB),
	REGNAME(dem, 0x2C, DBGBUSCLK_EN),
	REGNAME(dem, 0x30, DBGSYSCLK_EN),
	{},
};

static const struct regname *get_all_mt8189_regnames(void)
{
	return rn;
}

static void init_regbase(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rb) - 1; i++) {
		if (!rb[i].phys)
			continue;

		rb[i].virt = ioremap(rb[i].phys, 0x1000);
	}
}

u32 get_mt8189_reg_value(u32 id, u32 ofs)
{
	if (id >= chk_sys_num)
		return 0;

	return clk_readl(rb[id].virt + ofs);
}
EXPORT_SYMBOL_GPL(get_mt8189_reg_value);

/*
 * clkchk pwr_data
 */
struct pwr_data {
	const char *pvdname;
	enum chk_sys_id id;
	u32 base;
	u32 ofs;
};

/*
 * clkchk pwr_data
 */
static struct pwr_data pvd_pwr_data[] = {
	{"audiosys", afe, spm, 0x0E18},
	{"camsys_main", cam_m, spm, 0x0E48},
	{"camsys_rawa", cam_ra, spm, 0x0E50},
	{"camsys_rawb", cam_rb, spm, 0x0E54},
	{"dispsys", mm, spm, 0x0E70},
	{"imgsys1", imgsys1, spm, 0x0E28},
	{"imgsys2", imgsys2, spm, 0x0E2C},
	{"ipesys", ipe, spm, 0x0E30},
	{"mfgsys", mfg, spm, 0x0EB4},
	{"mm_infra", mminfra_config, spm, 0x0E78},
	{"ufs_pdn", ufscfg_pdn_reg, spm, 0x0E10},
	{"vdec_core", vdec_core, spm, 0x0E38},
	{"venc", ven1, spm, 0x0E40},
};

static int get_pvd_pwr_data_idx(const char *pvdname)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pvd_pwr_data); i++) {
		if (pvd_pwr_data[i].pvdname == NULL)
			continue;
		if (!strcmp(pvdname, pvd_pwr_data[i].pvdname))
			return i;
	}

	return -1;
}

static u32 pwr_ofs[STA_NUM] = {
	[PWR_STA] = 0xF40,
	[PWR_STA2] = 0xF44,
	[XPU_PWR_STA] = 0xF50,
	[XPU_PWR_STA2] = 0xF54,
};

u32 *get_spm_pwr_status_array(void)
{
	static void __iomem *scpsys_base, *pwr_addr[STA_NUM];
	static u32 pwr_sta[STA_NUM];
	int i;

	for (i = 0; i < STA_NUM; i++) {
		if (!scpsys_base)
			scpsys_base = ioremap(0x1c001000, PAGE_SIZE);

		if (pwr_ofs[i]) {
			pwr_addr[i] = scpsys_base + pwr_ofs[i];
			pwr_sta[i] = clk_readl(pwr_addr[i]);
		}
	}

	return pwr_sta;
}

static struct pvd_msk pvd_pwr_mask[] = {
	{"mfgsys", XPU_PWR_STA, 0x00000004},		// BIT(2), MFG1
	{"ufscfg_pdn", PWR_STA, 0x00000010},		// BIT(4), UFS0
	{"audiosys", PWR_STA, 0x00000040},		// BIT(6), AUDIO
	{"vadsys", PWR_STA, 0x00000080},		// BIT(7), ADSP_TOP
	{"imgsys1", PWR_STA, 0x00000400},		// BIT(10), ISP_IMG1
	{"imgsys2", PWR_STA, 0x00000800},		// BIT(11), ISP_IMG2
	{"ipesys", PWR_STA, 0x00001000},		// BIT(12), IPE
	{"vdec_core", PWR_STA, 0x00004000},		// BIT(14), VDE0
	{"venc", PWR_STA, 0x00010000},		// BIT(16), VEN0
	{"camsys_main", PWR_STA, 0x00040000},		// BIT(18), CAM_MAIN
	{"camsys_rawa", PWR_STA, 0x00100000},		// BIT(20), CAM_SUBA
	{"camsys_rawb", PWR_STA, 0x00200000},		// BIT(21), CAM_SUBB
	{"mdpsys", PWR_STA, 0x04000000},		// BIT(26), MDP0
	{"dispsys", PWR_STA, 0x10000000},		// BIT(28), DIS0
	{"mm_infra", PWR_STA, 0x40000000},		// BIT(30), MMINFRA
	{},
};

static struct pvd_msk *get_pvd_pwr_mask(void)
{
	return pvd_pwr_mask;
}

/*
 * clkchk pwr_status
 */
static u32 get_pwr_status(s32 idx)
{
	if (idx < 0 || idx >= ARRAY_SIZE(pvd_pwr_data))
		return 0;

	if (pvd_pwr_data[idx].id >= chk_sys_num)
		return 0;

	return  clk_readl(rb[pvd_pwr_data[idx].base].virt + pvd_pwr_data[idx].ofs);
}

static bool is_cg_chk_pwr_on(void)
{
#if CG_CHK_PWRON_ENABLE
	return true;
#endif
	return false;
}

#if CHECK_VCORE_FREQ
/*
 * clkchk vf table
 */

struct mtk_vf {
	const char *name;
	int freq_table[5];
};

#define MTK_VF_TABLE(_n, _freq0, _freq1, _freq2, _freq3, _freq4) {		\
		.name = _n,		\
		.freq_table = {_freq0, _freq1, _freq2, _freq3, _freq4},	\
	}

/*
 * Opp0 : 0p8v
 * Opp1 : 0p725v
 * Opp2 : 0p65v
 * Opp3 : 0p60v
 * Opp4 : 0p55v
 */
static struct mtk_vf vf_table[] = {
	/* Opp0, Opp1, Opp2, Opp3, Opp4 */
	MTK_VF_TABLE("axi_sel", 156000, 156000, 156000, 156000),
	MTK_VF_TABLE("axi_peri_sel", 156000, 156000, 156000, 156000),
	MTK_VF_TABLE("axi_u_sel", 78000, 78000, 78000, 78000),
	MTK_VF_TABLE("bus_aximem_sel", 364000, 273000, 273000, 218400),
	MTK_VF_TABLE("disp0_sel", 624000, 416000, 312000, 218400),
	MTK_VF_TABLE("mminfra_sel", 624000, 458333, 364000, 273000),
	MTK_VF_TABLE("uart_sel", 52000, 52000, 52000, 52000),
	MTK_VF_TABLE("spi0_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("spi1_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("spi2_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("spi3_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("spi4_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("spi5_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("msdc_macro_0p_sel", 416000, 416000, 416000, 416000),
	MTK_VF_TABLE("msdc5hclk_sel", 273000, 273000, 273000, 273000),
	MTK_VF_TABLE("msdc50_0_sel", 416000, 416000, 416000, 416000),
	MTK_VF_TABLE("aes_msdcfde_sel", 416000, 416000, 416000, 416000),
	MTK_VF_TABLE("msdc_macro_1p_sel", 416000, 416000, 416000, 416000),
	MTK_VF_TABLE("msdc30_1_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("msdc30_1_h_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("msdc_macro_2p_sel", 416000, 416000, 416000, 416000),
	MTK_VF_TABLE("msdc30_2_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("msdc30_2_h_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("aud_intbus_sel", 136500, 136500, 136500, 136500),
	MTK_VF_TABLE("atb_sel", 273000, 273000, 273000, 273000),
	MTK_VF_TABLE("disp_pwm_sel", 136500, 136500, 136500, 136500),
	MTK_VF_TABLE("usb_p0_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("ssusb_xhci_p0_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("usb_p1_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("ssusb_xhci_p1_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("usb_p2_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("ssusb_xhci_p2_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("usb_p3_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("ssusb_xhci_p3_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("usb_p4_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("ssusb_xhci_p4_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("i2c_sel", 136500, 136500, 136500, 136500),
	MTK_VF_TABLE("seninf_sel", 499200, 499200, 392857, 273000),
	MTK_VF_TABLE("seninf1_sel", 499200, 499200, 392857, 273000),
	MTK_VF_TABLE("aud_engen1_sel", 45158, 45158, 45158, 45158),
	MTK_VF_TABLE("aud_engen2_sel", 49152, 49152, 49152, 49152),
	MTK_VF_TABLE("aes_ufsfde_sel", 546000, 546000, 546000, 546000),
	MTK_VF_TABLE("ufs_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("ufs_mbist_sel", 297000, 297000, 297000, 297000),
	MTK_VF_TABLE("aud_1_sel", 180634, 180634, 180634, 180634),
	MTK_VF_TABLE("aud_2_sel", 196608, 196608, 196608, 196608),
	MTK_VF_TABLE("venc_sel", 624000, 458333, 343750, 249600),
	MTK_VF_TABLE("vdec_sel", 546000, 416000, 312000, 218400),
	MTK_VF_TABLE("pwm_sel", 78000, 78000, 78000, 78000),
	MTK_VF_TABLE("audio_h_sel", 196608, 196608, 196608, 196608),
	MTK_VF_TABLE("mcupm_sel", 218400, 218400, 218400, 218400),
	MTK_VF_TABLE("mem_sub_sel", 546000, 436800, 273000, 218400),
	MTK_VF_TABLE("mem_sub_peri_sel", 546000, 436800, 273000, 218400),
	MTK_VF_TABLE("mem_sub_u_sel", 546000, 436800, 273000, 218400),
	MTK_VF_TABLE("emi_n_sel", 688000, 688000, 688000, 688000),
	MTK_VF_TABLE("dsi_occ_sel", 312000, 312000, 249600, 208000),
	MTK_VF_TABLE("ap2conn_host_sel", 78000, 78000, 78000, 78000),
	MTK_VF_TABLE("img1_sel", 624000, 458333, 343750, 229167),
	MTK_VF_TABLE("ipe_sel", 546000, 416000, 312000, 229167),
	MTK_VF_TABLE("cam_sel", 624000, 546000, 392857, 273000),
	MTK_VF_TABLE("camtm_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("dsp_sel", 208000, 208000, 208000, 208000),
	MTK_VF_TABLE("sr_pka_sel", 436800, 312000, 273000, 136500),
	MTK_VF_TABLE("dxcc_sel", 273000, 273000, 273000, 273000),
	MTK_VF_TABLE("mfg_ref_sel", 364000, 364000, 364000, 364000),
	MTK_VF_TABLE("mdp0_sel", 624000, 416000, 312000, 218400),
	MTK_VF_TABLE("dp_sel", 297000, 148500, 148500, 148500),
	MTK_VF_TABLE("edp_sel", 297000, 148500, 148500, 148500),
	MTK_VF_TABLE("edp_favt_sel", 297000, 148500, 148500, 148500),
	MTK_VF_TABLE("snps_eth_250m_sel", 250000, 250000, 250000, 250000),
	MTK_VF_TABLE("snps_eth_62p4m_ptp_sel", 62500, 62500, 62500, 62500),
	MTK_VF_TABLE("snps_eth_50m_rmii_sel", 50000, 50000, 50000, 50000),
	MTK_VF_TABLE("sflash_sel", 124800, 124800, 124800, 124800),
	MTK_VF_TABLE("gcpu_sel", 416000, 364000, 364000, 273000),
	MTK_VF_TABLE("pcie_mac_tl_sel", 136500, 136500, 136500, 136500),
	MTK_VF_TABLE("vdstx_dg_cts_sel", 118900, 118900, 118900, 118900),
	MTK_VF_TABLE("pll_dpix_sel", 171900, 171900, 171900, 171900),
	MTK_VF_TABLE("ecc_sel", 546000, 416000, 416000, 312000),
	{},
};
#endif

static const char *get_vf_name(int id)
{
#if CHECK_VCORE_FREQ
	if (id < 0) {
		pr_info("[%s]Negative index detected\n", __func__);
		return NULL;
	}

	return vf_table[id].name;

#else
	return NULL;
#endif
}

static int get_vf_opp(int id, int opp)
{
#if CHECK_VCORE_FREQ
	if (id < 0 || opp < 0) {
		pr_info("[%s]Negative index detected\n", __func__);
		return 0;
	}

	return vf_table[id].freq_table[opp];
#else
	return 0;
#endif
}

static u32 get_vf_num(void)
{
#if CHECK_VCORE_FREQ
	return ARRAY_SIZE(vf_table) - 1;
#else
	return 0;
#endif
}

static int get_vcore_opp(void)
{
#if IS_ENABLED(CONFIG_MTK_DVFSRC_HELPER) && CHECK_VCORE_FREQ
	return mtk_dvfsrc_query_opp_info(MTK_DVFSRC_SW_REQ_VCORE_OPP);
#else
	return VCORE_NULL;
#endif
}

static unsigned int reg_dump_addr[ARRAY_SIZE(rn) - 1];
static unsigned int reg_dump_val[ARRAY_SIZE(rn) - 1];
static bool reg_dump_valid[ARRAY_SIZE(rn) - 1];

void set_subsys_reg_dump_mt8189(enum chk_sys_id id[])
{
	const struct regname *rns = &rn[0];
	int i, j, k;

	for (i = 0; i < ARRAY_SIZE(rn) - 1; i++, rns++) {
		int pwr_idx = PD_NULL;

		if (!is_valid_reg(ADDR(rns)))
			continue;

		for (j = 0; id[j] != chk_sys_num; j++) {
			/* filter out the subsys that we don't want */
			if (rns->id == id[j])
				break;
		}

		if (id[j] == chk_sys_num)
			continue;

		for (k = 0; k < ARRAY_SIZE(pvd_pwr_data); k++) {
			if (pvd_pwr_data[k].id == id[j]) {
				pwr_idx = k;
				break;
			}
		}

		if (pwr_idx != PD_NULL)
			if (!pwr_hw_is_on(PWR_CON_STA, pwr_idx))
				continue;

		reg_dump_addr[i] = PHYSADDR(rns);
		reg_dump_val[i] = clk_readl(ADDR(rns));
		/* record each register dump index validation */
		reg_dump_valid[i] = false;
	}
}
EXPORT_SYMBOL_GPL(set_subsys_reg_dump_mt8189);

void get_subsys_reg_dump_mt8189(void)
{
	const struct regname *rns = &rn[0];
	int i;

	for (i = 0; i < ARRAY_SIZE(rn) - 1; i++, rns++) {
		if (reg_dump_valid[i])
			pr_info("%-18s: [0x%08x] = 0x%08x\n",
					rns->name, reg_dump_addr[i], reg_dump_val[i]);
	}
}
EXPORT_SYMBOL_GPL(get_subsys_reg_dump_mt8189);

void print_subsys_reg_mt8189(enum chk_sys_id id)
{
	struct regbase *rb_dump;
	const struct regname *rns = &rn[0];
	int pwr_idx = PD_NULL;
	int i;

	if (id >= chk_sys_num) {
		pr_info("wrong id:%d\n", id);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(pvd_pwr_data); i++) {
		if (pvd_pwr_data[i].id == id) {
			pwr_idx = i;
			break;
		}
	}

	rb_dump = &rb[id];

	for (i = 0; i < ARRAY_SIZE(rn) - 1; i++, rns++) {
		if (!is_valid_reg(ADDR(rns)))
			return;

		/* filter out the subsys that we don't want */
		if (rns->base != rb_dump)
			continue;

		if (pwr_idx != PD_NULL) {
			if (!pwr_hw_is_on(PWR_CON_STA, pwr_idx))
				return;
		}

		pr_info("%-18s: [0x%08x] = 0x%08x\n",
			rns->name, PHYSADDR(rns), clk_readl(ADDR(rns)));
	}
}
EXPORT_SYMBOL_GPL(print_subsys_reg_mt8189);

static const char * const off_pll_names[] = {
	"mmpll",
	"mfgpll",
	"apll1",
	"apll2",
	"apupll2",
	"apupll",
	"tvdpll1",
	"tvdpll2",
	"ethpll",
	"msdcpll",
	"ufspll",
	NULL
};

static const char * const notice_pll_names[] = {
	NULL
};

static const char * const bypass_pll_name[] = {
	"mainpll",
	"univpll",
	"emipll",
	NULL
};

static const char * const *get_off_pll_names(void)
{
	return off_pll_names;
}

static const char * const *get_notice_pll_names(void)
{
	return notice_pll_names;
}

static const char * const *get_bypass_pll_name(void)
{
	return bypass_pll_name;
}

static bool is_pll_chk_bug_on(void)
{
#if (BUG_ON_CHK_ENABLE) || (IS_ENABLED(CONFIG_MTK_CLKMGR_DEBUG))
	return true;
#endif
	return false;
}

static bool is_suspend_retry_stop(bool reset_cnt)
{
	if (reset_cnt == true) {
		suspend_cnt = 0;
		return true;
	}

	suspend_cnt++;
	pr_notice("%s: suspend cnt: %d\n", __func__, suspend_cnt);

	if (suspend_cnt < 2)
		return false;

	return true;
}

static enum chk_sys_id bus_dump_id[] = {
	top,
	apmixed,
	chk_sys_num,
};

static void get_bus_reg(void)
{
	set_subsys_reg_dump_mt8189(bus_dump_id);
}

static void dump_bus_reg(struct regmap *regmap, u32 ofs)
{
	set_subsys_reg_dump_mt8189(bus_dump_id);
	get_subsys_reg_dump_mt8189();
	/* sspm need some time to run isr */
	mdelay(1000);
}

static enum chk_sys_id pll_dump_id[] = {
	apmixed,
	top,
	chk_sys_num,
};

static void dump_pll_reg(bool bug_on)
{
	set_subsys_reg_dump_mt8189(pll_dump_id);
	get_subsys_reg_dump_mt8189();

	if (bug_on) {
		mdelay(100);
		//BUG_ON(1);
	}
}

static bool clk_hw_is_on(struct clk_hw *hw)
{
	struct clk_hw *p_hw = clk_hw_get_parent(hw);

	if (p_hw && !clk_hw_is_enabled(p_hw))
		return false;

	return clk_hw_is_enabled(hw) || clk_hw_is_prepared(hw);
}

static bool pvdck_is_on(struct provider_clk *pvdck)
{
	struct clk *c = NULL;
	struct clk_hw *c_hw = NULL;

	if (!pvdck)
		return false;

	c = pvdck->ck;
	c_hw = __clk_get_hw(c);

	if (!c_hw)
		return false;

	/* this clock depends on infra mtcmos */
	if (pvdck->pwr_mask == INV_MSK || !pvdck->pwr_mask)
		/* check the clk hardware status directly */
		return clk_hw_is_on(c_hw);
	/* this clock depends on non-infra mtcmos */
	else {
		/* if mtcmos is on, then check the clk hardware status */
		if (pwr_hw_is_on(pvdck->sta_type, pvdck->pwr_mask) > 0)
			return clk_hw_is_on(c_hw);
		/* if mtcmos is off, then return 0 directly */
		else
			return false;
	}
}

/*
 * init functions
 */

static struct clkchk_ops clkchk_mt8189_ops = {
	.get_all_regnames = get_all_mt8189_regnames,
	.get_pvd_pwr_data_idx = get_pvd_pwr_data_idx,
	.get_pwr_status = get_pwr_status,
	.is_cg_chk_pwr_on = is_cg_chk_pwr_on,
	.get_off_pll_names = get_off_pll_names,
	.get_notice_pll_names = get_notice_pll_names,
	.get_bypass_pll_name = get_bypass_pll_name,
	.is_pll_chk_bug_on = is_pll_chk_bug_on,
	.get_vf_name = get_vf_name,
	.get_vf_opp = get_vf_opp,
	.get_vf_num = get_vf_num,
	.get_vcore_opp = get_vcore_opp,
	.get_bus_reg = get_bus_reg,
	.dump_bus_reg = dump_bus_reg,
	.dump_pll_reg = dump_pll_reg,
	.trace_clk_event = trace_clk_event,
	.is_suspend_retry_stop = is_suspend_retry_stop,
	.get_spm_pwr_status_array = get_spm_pwr_status_array,
	.is_pwr_on = pvdck_is_on,
	.get_pvd_pwr_mask = get_pvd_pwr_mask,
};

static int clk_chk_mt8189_probe(struct platform_device *pdev)
{
	suspend_cnt = 0;

	init_regbase();

	set_clkchk_notify();

	set_clkchk_ops(&clkchk_mt8189_ops);

#if CHECK_VCORE_FREQ
	mtk_clk_check_muxes();
#endif

	return 0;
}

static const struct of_device_id of_match_clkchk_mt8189[] = {
	{
		.compatible = "mediatek,mt8189-clkchk",
	}, {
		/* sentinel */
	}
};

static struct platform_driver clk_chk_mt8189_drv = {
	.probe = clk_chk_mt8189_probe,
	.driver = {
		.name = "clk-chk-mt8189",
		.owner = THIS_MODULE,
		.pm = &clk_chk_dev_pm_ops,
		.of_match_table = of_match_clkchk_mt8189,
	},
};

/*
 * init functions
 */

static int __init clkchk_mt8189_init(void)
{
	return platform_driver_register(&clk_chk_mt8189_drv);
}

static void __exit clkchk_mt8189_exit(void)
{
	platform_driver_unregister(&clk_chk_mt8189_drv);
}

subsys_initcall(clkchk_mt8189_init);
module_exit(clkchk_mt8189_exit);
MODULE_LICENSE("GPL");
