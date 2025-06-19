/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright 2024, Beijing ESWIN Computing Technology Co., Ltd.. All rights reserved.
 *
 * Device Tree binding constants for EIC7700 reset controller.
 *
 * Authors:
 *	Yifeng Huang <huangyifeng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 */

#ifndef __DT_ESWIN_EIC7700_RESET_H__
#define __DT_ESWIN_EIC7700_RESET_H__

#define SNOC_RST_CTRL 0
#define GPU_RST_CTRL 1
#define DSP_RST_CTRL 2
#define D2D_RST_CTRL 3
#define DDR_RST_CTRL 4
#define TCU_RST_CTRL 5
#define NPU_RST_CTRL 6
#define HSPDMA_RST_CTRL 7
#define PCIE_RST_CTRL 8
#define I2C_RST_CTRL 9
#define FAN_RST_CTRL 10
#define PVT_RST_CTRL 11
#define MBOX_RST_CTRL 12
#define UART_RST_CTRL 13
#define GPIO_RST_CTRL 14
#define TIMER_RST_CTRL 15
#define SSI_RST_CTRL 16
#define WDT_RST_CTRL 17
#define LSP_CFGRST_CTRL 18
#define U84_RST_CTRL 19
#define SCPU_RST_CTRL 20
#define LPCPU_RST_CTRL 21
#define VC_RST_CTRL 22
#define JD_RST_CTRL 23
#define JE_RST_CTRL 24
#define VD_RST_CTRL 25
#define VE_RST_CTRL 26
#define G2D_RST_CTRL 27
#define VI_RST_CTRL 28
#define DVP_RST_CTRL 29
#define ISP0_RST_CTRL 30
#define ISP1_RST_CTRL 31
#define SHUTTER_RST_CTRL 32
#define VO_PHYRST_CTRL 33
#define VO_I2SRST_CTRL 34
#define VO_RST_CTRL 35
#define BOOTSPI_RST_CTRL 36
#define I2C1_RST_CTRL 37
#define I2C0_RST_CTRL 38
#define DMA1_RST_CTRL 39
#define FPRT_RST_CTRL 40
#define HBLOCK_RST_CTRL 41
#define SECSR_RST_CTRL 42
#define OTP_RST_CTRL 43
#define PKA_RST_CTRL 44
#define SPACC_RST_CTRL 45
#define TRNG_RST_CTRL 46
#define RESERVED 47
#define TIMER0_RST_CTRL 48
#define TIMER1_RST_CTRL 49
#define TIMER2_RST_CTRL 50
#define TIMER3_RST_CTRL 51
#define RTC_RST_CTRL 52
#define MNOC_RST_CTRL 53
#define RNOC_RST_CTRL 54
#define CNOC_RST_CTRL 55
#define LNOC_RST_CTRL 56

/*
 * CONSUMER RESET CONTROL BIT
 */
/*SNOC*/
#define SW_NOC_NSP_RSTN 0
#define SW_NOC_CFG_RSTN 1
#define SW_RNOC_NSP_RSTN 2
#define SW_SNOC_TCU_ARSTN 3
#define SW_SNOC_U84_ARSTN 4
#define SW_SNOC_PCIET_XSRSTN 5
#define SW_SNOC_PCIET_XMRSTN 6
#define SW_SNOC_PCIET_PRSTN 7
#define SW_SNOC_NPU_ARSTN 8
#define SW_SNOC_JTAG_ARSTN 9
#define SW_SNOC_DSPT_ARSTN 10
#define SW_SNOC_DDRC1_P2_ARSTN 11
#define SW_SNOC_DDRC1_P1_ARSTN 12
#define SW_SNOC_DDRC0_P2_ARSTN 13
#define SW_SNOC_DDRC0_P1_ARSTN 14
#define SW_SNOC_D2D_ARSTN 15
#define SW_SNOC_AON_ARSTN 16

/*GPU*/
#define SW_GPU_AXI_RSTN 0
#define SW_GPU_CFG_RSTN 1
#define SW_GPU_GRAY_RSTN 2
#define SW_GPU_JONES_RSTN 3
#define SW_GPU_SPU_RSTN 4

/*DSP*/
#define SW_DSP_AXI_RSTN 0
#define SW_DSP_CFG_RSTN 1
#define SW_DSP_DIV4_RSTN 2
#define SW_DSP_DIV_RSTN_0 4
#define SW_DSP_DIV_RSTN_1 5
#define SW_DSP_DIV_RSTN_2 6
#define SW_DSP_DIV_RSTN_3 7

/*D2D*/
#define SW_D2D_AXI_RSTN 0
#define SW_D2D_CFG_RSTN 1
#define SW_D2D_PRST_N 2
#define SW_D2D_RAW_PCS_RST_N 4
#define SW_D2D_RX_RST_N 5
#define SW_D2D_TX_RST_N 6
#define SW_D2D_CORE_RST_N 7

/*TCU*/
#define SW_TCU_AXI_RSTN 0
#define SW_TCU_CFG_RSTN 1
#define TBU_RSTN_0 4
#define TBU_RSTN_1 5
#define TBU_RSTN_2 6
#define TBU_RSTN_3 7
#define TBU_RSTN_4 8
#define TBU_RSTN_5 9
#define TBU_RSTN_6 10
#define TBU_RSTN_7 11
#define TBU_RSTN_8 12
#define TBU_RSTN_9 13
#define TBU_RSTN_10 14
#define TBU_RSTN_11 15
#define TBU_RSTN_12 16
#define TBU_RSTN_13 17
#define TBU_RSTN_14 18
#define TBU_RSTN_15 19
#define TBU_RSTN_16 20

/*NPU*/
#define SW_NPU_AXI_RSTN 0
#define SW_NPU_CFG_RSTN 1
#define SW_NPU_CORE_RSTN 2
#define SW_NPU_E31CORE_RSTN 3
#define SW_NPU_E31BUS_RSTN 4
#define SW_NPU_E31DBG_RSTN 5
#define SW_NPU_LLC_RSTN 6

/*HSP DMA*/
#define SW_HSP_AXI_RSTN 0
#define SW_HSP_CFG_RSTN 1
#define SW_HSP_POR_RSTN 2
#define SW_MSHC0_PHY_RSTN 3
#define SW_MSHC1_PHY_RSTN 4
#define SW_MSHC2_PHY_RSTN 5
#define SW_MSHC0_TXRX_RSTN 6
#define SW_MSHC1_TXRX_RSTN 7
#define SW_MSHC2_TXRX_RSTN 8
#define SW_SATA_ASIC0_RSTN 9
#define SW_SATA_OOB_RSTN 10
#define SW_SATA_PMALIVE_RSTN 11
#define SW_SATA_RBC_RSTN 12
#define SW_DMA0_RST_N 13
#define SW_HSP_DMA0_RSTN 14
#define SW_USB0_VAUX_RSTN 15
#define SW_USB1_VAUX_RSTN 16
#define SW_HSP_SD1_PRSTN 17
#define SW_HSP_SD0_PRSTN 18
#define SW_HSP_EMMC_PRSTN 19
#define SW_HSP_DMA_PRSTN 20
#define SW_HSP_SD1_ARSTN 21
#define SW_HSP_SD0_ARSTN 22
#define SW_HSP_EMMC_ARSTN 23
#define SW_HSP_DMA_ARSTN 24
#define SW_HSP_ETH1_ARSTN 25
#define SW_HSP_ETH0_ARSTN 26
#define SW_HSP_SATA_ARSTN 27

/*PCIE*/
#define SW_PCIE_CFG_RSTN 0
#define SW_PCIE_POWERUP_RSTN 1
#define SW_PCIE_PERST_N 2

/*I2C*/
#define SW_I2C_RST_N_0 0
#define SW_I2C_RST_N_1 1
#define SW_I2C_RST_N_2 2
#define SW_I2C_RST_N_3 3
#define SW_I2C_RST_N_4 4
#define SW_I2C_RST_N_5 5
#define SW_I2C_RST_N_6 6
#define SW_I2C_RST_N_7 7
#define SW_I2C_RST_N_8 8
#define SW_I2C_RST_N_9 9

/*FAN*/
#define SW_FAN_RST_N 0

/*PVT*/
#define SW_PVT_RST_N_0 0
#define SW_PVT_RST_N_1 1

/*MBOX*/
#define SW_MBOX_RST_N_0 0
#define SW_MBOX_RST_N_1 1
#define SW_MBOX_RST_N_2 2
#define SW_MBOX_RST_N_3 3
#define SW_MBOX_RST_N_4 4
#define SW_MBOX_RST_N_5 5
#define SW_MBOX_RST_N_6 6
#define SW_MBOX_RST_N_7 7
#define SW_MBOX_RST_N_8 8
#define SW_MBOX_RST_N_9 9
#define SW_MBOX_RST_N_10 10
#define SW_MBOX_RST_N_11 11
#define SW_MBOX_RST_N_12 12
#define SW_MBOX_RST_N_13 13
#define SW_MBOX_RST_N_14 14
#define SW_MBOX_RST_N_15 15

/*UART*/
#define SW_UART_RST_N_0 0
#define SW_UART_RST_N_1 1
#define SW_UART_RST_N_2 2
#define SW_UART_RST_N_3 3
#define SW_UART_RST_N_4 4

/*GPIO*/
#define SW_GPIO_RST_N_0 0
#define SW_GPIO_RST_N_1 1

/*TIMER*/
#define SW_TIMER_RST_N 0

/*SSI*/
#define SW_SSI_RST_N_0 0
#define SW_SSI_RST_N_1 1

/*WDT*/
#define SW_WDT_RST_N_0 0
#define SW_WDT_RST_N_1 1
#define SW_WDT_RST_N_2 2
#define SW_WDT_RST_N_3 3

/*LSP CFG*/
#define SW_LSP_CFG_RSTN 0

/*U84 CFG*/
#define SW_U84_CORE_RSTN_0 0
#define SW_U84_CORE_RSTN_1 1
#define SW_U84_CORE_RSTN_2 2
#define SW_U84_CORE_RSTN_3 3
#define SW_U84_BUS_RSTN 4
#define SW_U84_DBG_RSTN 5
#define SW_U84_TRACECOM_RSTN 6
#define SW_U84_TRACE_RSTN_0 8
#define SW_U84_TRACE_RSTN_1 9
#define SW_U84_TRACE_RSTN_2 10
#define SW_U84_TRACE_RSTN_3 11

/*SCPU*/
#define SW_SCPU_CORE_RSTN 0
#define SW_SCPU_BUS_RSTN 1
#define SW_SCPU_DBG_RSTN 2

/*LPCPU*/
#define SW_LPCPU_CORE_RSTN 0
#define SW_LPCPU_BUS_RSTN 1
#define SW_LPCPU_DBG_RSTN 2

/*VC*/
#define SW_VC_CFG_RSTN 0
#define SW_VC_AXI_RSTN 1
#define SW_VC_MONCFG_RSTN 2

/*JD*/
#define SW_JD_CFG_RSTN 0
#define SW_JD_AXI_RSTN 1

/*JE*/
#define SW_JE_CFG_RSTN 0
#define SW_JE_AXI_RSTN 1

/*VD*/
#define SW_VD_CFG_RSTN 0
#define SW_VD_AXI_RSTN 1

/*VE*/
#define SW_VE_AXI_RSTN 0
#define SW_VE_CFG_RSTN 1

/*G2D*/
#define SW_G2D_CORE_RSTN 0
#define SW_G2D_CFG_RSTN 1
#define SW_G2D_AXI_RSTN 2

/*VI*/
#define SW_VI_AXI_RSTN 0
#define SW_VI_CFG_RSTN 1
#define SW_VI_DWE_RSTN 2

/*DVP*/
#define SW_VI_DVP_RSTN 0

/*ISP0*/
#define SW_VI_ISP0_RSTN 0

/*ISP1*/
#define SW_VI_ISP1_RSTN 0

/*SHUTTR*/
#define SW_VI_SHUTTER_RSTN_0 0
#define SW_VI_SHUTTER_RSTN_1 1
#define SW_VI_SHUTTER_RSTN_2 2
#define SW_VI_SHUTTER_RSTN_3 3
#define SW_VI_SHUTTER_RSTN_4 4
#define SW_VI_SHUTTER_RSTN_5 5

/*VO PHY*/
#define SW_VO_MIPI_PRSTN 0
#define SW_VO_PRSTN 1
#define SW_VO_HDMI_PRSTN 3
#define SW_HDMI_PHYCTRL_RSTN 4
#define SW_VO_HDMI_RSTN 5

/*VO I2S*/
#define SW_VO_I2S_RSTN 0
#define SW_VO_I2S_PRSTN 1

/*VO*/
#define SW_VO_AXI_RSTN 0
#define SW_VO_CFG_RSTN 1
#define SW_VO_DC_RSTN 2
#define SW_VO_DC_PRSTN 3

/*BOOTSPI*/
#define SW_BOOTSPI_HRSTN 0
#define SW_BOOTSPI_RSTN 1

/*I2C1*/
#define SW_I2C1_PRSTN 0

/*I2C0*/
#define SW_I2C0_PRSTN 0

/*DMA1*/
#define SW_DMA1_ARSTN 0
#define SW_DMA1_HRSTN 1

/*FPRT*/
#define SW_FP_PRT_HRSTN 0

/*HBLOCK*/
#define SW_HBLOCK_HRSTN 0

/*SECSR*/
#define SW_SECSR_HRSTN 0

/*OTP*/
#define SW_OTP_PRSTN 0

/*PKA*/
#define SW_PKA_HRSTN 0

/*SPACC*/
#define SW_SPACC_RSTN 0

/*TRNG*/
#define SW_TRNG_HRSTN 0

/*TIMER0*/
#define SW_TIMER0_RSTN_0 0
#define SW_TIMER0_RSTN_1 1
#define SW_TIMER0_RSTN_2 2
#define SW_TIMER0_RSTN_3 3
#define SW_TIMER0_RSTN_4 4
#define SW_TIMER0_RSTN_5 5
#define SW_TIMER0_RSTN_6 6
#define SW_TIMER0_RSTN_7 7
#define SW_TIMER0_PRSTN 8

/*TIMER1*/
#define SW_TIMER1_RSTN_0 0
#define SW_TIMER1_RSTN_1 1
#define SW_TIMER1_RSTN_2 2
#define SW_TIMER1_RSTN_3 3
#define SW_TIMER1_RSTN_4 4
#define SW_TIMER1_RSTN_5 5
#define SW_TIMER1_RSTN_6 6
#define SW_TIMER1_RSTN_7 7
#define SW_TIMER1_PRSTN 8

/*TIMER2*/
#define SW_TIMER2_RSTN_0 0
#define SW_TIMER2_RSTN_1 1
#define SW_TIMER2_RSTN_2 2
#define SW_TIMER2_RSTN_3 3
#define SW_TIMER2_RSTN_4 4
#define SW_TIMER2_RSTN_5 5
#define SW_TIMER2_RSTN_6 6
#define SW_TIMER2_RSTN_7 7
#define SW_TIMER2_PRSTN 8

/*TIMER3*/
#define SW_TIMER3_RSTN_0 0
#define SW_TIMER3_RSTN_1 1
#define SW_TIMER3_RSTN_2 2
#define SW_TIMER3_RSTN_3 3
#define SW_TIMER3_RSTN_4 4
#define SW_TIMER3_RSTN_5 5
#define SW_TIMER3_RSTN_6 6
#define SW_TIMER3_RSTN_7 7
#define SW_TIMER3_PRSTN 8

/*RTC*/
#define SW_RTC_RSTN 0

/*MNOC*/
#define SW_MNOC_SNOC_NSP_RSTN 0
#define SW_MNOC_VC_ARSTN 1
#define SW_MNOC_CFG_RSTN 2
#define SW_MNOC_HSP_ARSTN 3
#define SW_MNOC_GPU_ARSTN 4
#define SW_MNOC_DDRC1_P3_ARSTN 5
#define SW_MNOC_DDRC0_P3_ARSTN 6

/*RNOC*/
#define SW_RNOC_VO_ARSTN 0
#define SW_RNOC_VI_ARSTN 1
#define SW_RNOC_SNOC_NSP_RSTN 2
#define SW_RNOC_CFG_RSTN 3
#define SW_MNOC_DDRC1_P4_ARSTN 4
#define SW_MNOC_DDRC0_P4_ARSTN 5

/*CNOC*/
#define SW_CNOC_VO_CFG_RSTN 0
#define SW_CNOC_VI_CFG_RSTN 1
#define SW_CNOC_VC_CFG_RSTN 2
#define SW_CNOC_TCU_CFG_RSTN 3
#define SW_CNOC_PCIET_CFG_RSTN 4
#define SW_CNOC_NPU_CFG_RSTN 5
#define SW_CNOC_LSP_CFG_RSTN 6
#define SW_CNOC_HSP_CFG_RSTN 7
#define SW_CNOC_GPU_CFG_RSTN 8
#define SW_CNOC_DSPT_CFG_RSTN 9
#define SW_CNOC_DDRT1_CFG_RSTN 10
#define SW_CNOC_DDRT0_CFG_RSTN 11
#define SW_CNOC_D2D_CFG_RSTN 12
#define SW_CNOC_CFG_RSTN 13
#define SW_CNOC_CLMM_CFG_RSTN 14
#define SW_CNOC_AON_CFG_RSTN 15

/*LNOC*/
#define SW_LNOC_CFG_RSTN 0
#define SW_LNOC_NPU_LLC_ARSTN 1
#define SW_LNOC_DDRC1_P0_ARSTN 2
#define SW_LNOC_DDRC0_P0_ARSTN 3

#endif /*endif __DT_ESWIN_EIC7700_RESET_H__*/
