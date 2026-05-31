/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Qualcomm MSM8x60 family (MSM8260/MSM8660/APQ8060) interconnect IDs
 *
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * Based on webOS kernel msm_bus_board_8660.c
 * Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 */

#ifndef __DT_BINDINGS_INTERCONNECT_QCOM_MSM8660_H
#define __DT_BINDINGS_INTERCONNECT_QCOM_MSM8660_H

/*
 * MSM8x60 has a fabric-based bus architecture:
 * - APPSS Fabric: CPU and memory interface
 * - System Fabric: System peripherals and DMA
 * - MMSS Fabric: Multimedia subsystem (display, camera, video)
 * - Daytona Fabric: Peripheral bus (SDCC, ADM DMA)
 * - System FPB: System Fast Peripheral Bus
 * - CPSS FPB: CPU Subsystem Fast Peripheral Bus
 */

/* APPSS Fabric - Apps processor fabric */
#define AFAB_MAS_AMPSS_M0		0
#define AFAB_MAS_AMPSS_M1		1
#define AFAB_SLV_EBI_CH0		2
#define AFAB_SLV_AMPSS_L2		3
#define AFAB_TO_MMSS			4
#define AFAB_TO_SYSTEM			5

/* System Fabric - System bus */
#define SFAB_MAS_APPSS			0
#define SFAB_MAS_SPS			1
#define SFAB_MAS_ADM0_PORT0		2
#define SFAB_MAS_ADM0_PORT1		3
#define SFAB_MAS_ADM1_PORT0		4
#define SFAB_MAS_ADM1_PORT1		5
#define SFAB_MAS_LPASS_PROC		6
#define SFAB_MAS_MSS_PROCI		7
#define SFAB_MAS_MSS_PROCD		8
#define SFAB_MAS_MSS_MDM_PORT0		9
#define SFAB_MAS_LPASS			10
#define SFAB_MAS_MMSS_FPB		11
#define SFAB_MAS_ADM1_CI		12
#define SFAB_MAS_ADM0_CI		13
#define SFAB_MAS_MSS_MDM_PORT1		14
#define SFAB_MAS_USB_HS			15
#define SFAB_TO_APPSS			16
#define SFAB_TO_SYSTEM_FPB		17
#define SFAB_TO_CPSS_FPB		18
#define SFAB_SLV_SPS			19
#define SFAB_SLV_SYSTEM_IMEM		20
#define SFAB_SLV_AMPSS			21
#define SFAB_SLV_MSS			22
#define SFAB_SLV_LPASS			23
#define SFAB_SLV_MMSS_FPB		24
#define SFAB_TO_DFAB			25

/* MMSS Fabric - Multimedia subsystem */
#define MMFAB_MAS_MDP_PORT0		0
#define MMFAB_MAS_MDP_PORT1		1
#define MMFAB_MAS_ADM1_PORT0		2
#define MMFAB_MAS_ROTATOR		3
#define MMFAB_MAS_GRAPHICS_3D		4
#define MMFAB_MAS_JPEG_DEC		5
#define MMFAB_MAS_GRAPHICS_2D_CORE0	6
#define MMFAB_MAS_VFE			7
#define MMFAB_MAS_VPE			8
#define MMFAB_MAS_JPEG_ENC		9
#define MMFAB_MAS_GRAPHICS_2D_CORE1	10
#define MMFAB_MAS_HD_CODEC_PORT0	11
#define MMFAB_MAS_HD_CODEC_PORT1	12
#define MMFAB_TO_APPSS			13
#define MMFAB_SLV_SMI			14
#define MMFAB_SLV_MM_IMEM		15

/*
 * Daytona Fabric (DFAB) - Peripheral bus
 *
 * DFAB connects slower peripherals (SDCC, ADM DMA) to the system fabric.
 * The webOS kernel managed DFAB bandwidth via voter clocks (dfab_sdc*_clk,
 * dfab_usb_hs_clk). In mainline, this is handled by the interconnect framework.
 *
 * USB HS is included as a DFAB voter for compatibility with the legacy clock
 * voting mechanism. The webOS kernel comment said: "if usb link is in sps
 * there is no need for usb pclk as daytona fabric clock will be used instead".
 * This keeps DFAB clock stable when USB is active.
 */
#define DFAB_MAS_SDC1			0
#define DFAB_MAS_SDC2			1
#define DFAB_MAS_SDC3			2
#define DFAB_MAS_SDC4			3
#define DFAB_MAS_SDC5			4
#define DFAB_MAS_ADM0_MASTER		5
#define DFAB_MAS_ADM1_MASTER		6
#define DFAB_TO_SFAB			7
#define DFAB_SLV_SDC1			8
#define DFAB_SLV_SDC2			9
#define DFAB_SLV_SDC3			10
#define DFAB_SLV_SDC4			11
#define DFAB_SLV_SDC5			12
#define DFAB_MAS_USB_HS			13
#define DFAB_MAS_DSPS			14

/* System FPB - Slow peripheral bus for system */
#define SFPB_MAS_SYSTEM			0
#define SFPB_MAS_SPDM			1
#define SFPB_MAS_RPM			2
#define SFPB_SLV_SPDM			3
#define SFPB_SLV_RPM			4
#define SFPB_SLV_RPM_MSG_RAM		5
#define SFPB_SLV_MPM			6
#define SFPB_SLV_PMIC1_SSBI1_A		7
#define SFPB_SLV_PMIC1_SSBI1_B		8
#define SFPB_SLV_PMIC1_SSBI1_C		9
#define SFPB_SLV_PMIC2_SSBI2_A		10
#define SFPB_SLV_PMIC2_SSBI2_B		11

/* CPSS FPB - CPU subsystem fast peripheral bus */
#define CFPB_MAS_SYSTEM			0
#define CFPB_SLV_GSBI1_UART		1
#define CFPB_SLV_GSBI2_UART		2
#define CFPB_SLV_GSBI3_UART		3
#define CFPB_SLV_GSBI4_UART		4
#define CFPB_SLV_GSBI5_UART		5
#define CFPB_SLV_GSBI6_UART		6
#define CFPB_SLV_GSBI7_UART		7
#define CFPB_SLV_GSBI8_UART		8
#define CFPB_SLV_GSBI9_UART		9
#define CFPB_SLV_GSBI10_UART		10
#define CFPB_SLV_GSBI11_UART		11
#define CFPB_SLV_GSBI12_UART		12
#define CFPB_SLV_GSBI1_QUP		13
#define CFPB_SLV_GSBI2_QUP		14
#define CFPB_SLV_GSBI3_QUP		15
#define CFPB_SLV_GSBI4_QUP		16
#define CFPB_SLV_GSBI5_QUP		17
#define CFPB_SLV_GSBI6_QUP		18
#define CFPB_SLV_GSBI7_QUP		19
#define CFPB_SLV_GSBI8_QUP		20
#define CFPB_SLV_GSBI9_QUP		21
#define CFPB_SLV_GSBI10_QUP		22
#define CFPB_SLV_GSBI11_QUP		23
#define CFPB_SLV_GSBI12_QUP		24
#define CFPB_SLV_EBI2_NAND		25
#define CFPB_SLV_USB_FS1		26
#define CFPB_SLV_USB_FS2		27
#define CFPB_SLV_TSIF			28
#define CFPB_SLV_MSM_TSSC		29
#define CFPB_SLV_MSM_PDM		30
#define CFPB_SLV_MSM_DIMEM		31
#define CFPB_SLV_MSM_TCSR		32
#define CFPB_SLV_MSM_PRNG		33

#endif /* __DT_BINDINGS_INTERCONNECT_QCOM_MSM8660_H */
