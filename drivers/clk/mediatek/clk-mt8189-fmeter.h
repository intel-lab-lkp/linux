/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#ifndef _CLK_MT8189_FMETER_H
#define _CLK_MT8189_FMETER_H

/* generate from clock_table.xlsx from TOPCKGEN DE */

/* CKGEN Part */
#define FM_AXI_CK				1
#define FM_AXI_PERI_CK				2
#define FM_AXI_U_CK				3
#define FM_BUS_AXIMEM_CK			4
#define FM_DISP0_CK				5
#define FM_MMINFRA_CK				6
#define FM_UART_CK				7
#define FM_SPI0_CK				8
#define FM_SPI1_CK				9
#define FM_SPI2_CK				10
#define FM_SPI3_CK				11
#define FM_SPI4_CK				12
#define FM_SPI5_CK				13
#define FM_MSDC_MACRO_0P_CK			14
#define FM_MSDC5HCLK_CK				15
#define FM_MSDC50_0_CK				16
#define FM_AES_MSDCFDE_CK			17
#define FM_MSDC_MACRO_1P_CK			18
#define FM_MSDC30_1_CK				19
#define FM_MSDC30_1_H_CK			20
#define FM_MSDC_MACRO_2P_CK			21
#define FM_MSDC30_2_CK				22
#define FM_MSDC30_2_2				23
#define FM_AUD_INTBUS_CK			24
#define FM_ATB_CK				25
#define FM_DISP_PWM_CK				26
#define FM_USB_P0_CK				27
#define FM_USB_XHCI_P0_CK			28
#define FM_USB_P1_CK				29
#define FM_USB_XHCI_P1_CK			30
#define FM_USB_P2_CK				31
#define FM_USB_XHCI_P2_CK			32
#define FM_USB_P3_CK				33
#define FM_USB_XHCI_P3_CK			34
#define FM_USB_P4_CK				35
#define FM_USB_XHCI_P4_CK			36
#define FM_I2C_CK				37
#define FM_SENINF_CK				38
#define FM_SENINF1_CK				39
#define FM_AUD_ENGEN1_CK			40
#define FM_AUD_ENGEN2_CK			41
#define FM_AES_UFSFDE_CK			42
#define FM_U_CK					43
#define FM_U_MBIST_CK				44
#define FM_AUD_1_CK				45
#define FM_AUD_2_CK				46
#define FM_VENC_CK				47
#define FM_VDEC_CK				48
#define FM_PWM_CK				49
#define FM_AUDIO_H_CK				50
#define FM_MCUPM_CK				51
#define FM_MEM_SUB_CK				52
#define FM_MEM_SUB_PERI_CK			53
#define FM_MEM_SUB_U_CK				54
#define FM_EMI_N_CK				55
#define FM_DSI_OCC_CK				56
#define FM_AP2CONN_HOST_CK			57
#define FM_IMG1_CK				58
#define FM_IPE_CK				59
#define FM_CAM_CK				60
#define FM_CAMTM_CK				61
#define FM_DSP_CK				62
#define FM_SR_PKA_CK				63
#define FM_DXCC_CK				64
#define FM_MFG_REF_CK				65
#define FM_MDP0_CK				66
#define FM_DP_CK				67
#define FM_EDP_CK				68
#define FM_EDP_FAVT_CK				69
#define FM_ETH_250M_CK				70
#define FM_ETH_62P4M_PTP_CK			71
#define FM_ETH_50M_RMII_CK			72
#define FM_SFLASH_CK				73
#define FM_GCPU_CK				74
#define FM_CIE_MAC_TL_CK			75
#define FM_VDSTX_DG_CTS_CK			76
#define FM_PLL_DPIX_CK				77
#define FM_ECC_CK				78
#define FM_CKGEN_NUM				79
/* ABIST Part */
#define FM_APLL1_CK				2
#define FM_APLL2_CK				3
#define FM_ARMPLL_BL_CK				6
#define FM_ARMPLL_BL_CKDIV_CK			7
#define FM_ARMPLL_LL_CK				8
#define FM_ARMPLL_LL_CKDIV_CK			9
#define FM_CCIPLL_CK				10
#define FM_CCIPLL_CKDIV_CK			11
#define FM_MAINPLL_CKDIV_CK			23
#define FM_MAINPLL_CK				24
#define FM_MMPLL_CKDIV_CK			26
#define FM_MMPLL_CK				27
#define FM_MMPLL_D3_CK				28
#define FM_MSDCPLL_CK				30
#define FM_UFSPLL_CK				35
#define FM_UNIVPLL_CK				38
#define FM_UNIVPLL_192M_CK			40
#define FM_APLL1_CKDIV_CK			71
#define FM_APLL2_CKDIV_CK			72
#define FM_UFSPLL_CKDIV_CK			74
#define FM_MSDCPLL_CKDIV_CK			77
#define FM_EMIPLL_CK				78
#define FM_TVDPLL1_CK				79
#define FM_TVDPLL2_CK				80
#define FM_MFGPLL_OPP_CK			81
#define FM_ETHPLL_CK				82
#define FM_APUPLL_CK				83
#define FM_APUPLL2_CK				84
#define FM_ABIST_NUM				85
/* VLPCK Part */
#define FM_SCP_CK				1
#define FM_PWRAP_ULPOSC_CK			2
#define FM_SPMI_P_CK				3
#define FM_DVFSRC_CK				4
#define FM_PWM_VLP_CK				5
#define FM_AXI_VLP_CK				6
#define FM_SYSTIMER_26M_CK			7
#define FM_SSPM_CK				8
#define FM_SSPM_F26M_CK				9
#define FM_SRCK_CK				10
#define FM_SCP_SPI_CK				11
#define FM_SCP_IIC_CK				12
#define FM_SCP_SPI_HS_CK			13
#define FM_SCP_IIC_HS_CK			14
#define FM_SSPM_ULPOSC_CK			15
#define FM_APXGPT_26M_CK			16
#define FM_VADSP_CK				17
#define FM_VADSP_VOWPLL_CK			18
#define FM_VADSP_UARTHUB_B_CK			19
#define FM_CAMTG0_CK				20
#define FM_CAMTG1_CK				21
#define FM_CAMTG2_CK				22
#define FM_AUD_ADC_CK				23
#define FM_KP_IRQ_GEN_CK			24
#define FM_VLPCK_NUM				25

enum fm_sys_id {
	FM_APMIXEDSYS = 0,
	FM_TOPCKGEN = 1,
	FM_VLP_CKSYS = 2,
	FM_SYS_NUM = 3,
};

#endif /* _CLK_MT8189_FMETER_H */
