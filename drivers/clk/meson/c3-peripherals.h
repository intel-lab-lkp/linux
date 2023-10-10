/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2023 Amlogic, inc.
 * Author: Chuan Liu <chuan.liu@amlogic.com>
 */

#ifndef __AML_C3_PERIPHERALS_H__
#define __AML_C3_PERIPHERALS_H__

#define OSCIN_CTRL				0x0004
#define RTC_BY_OSCIN_CTRL0			0x0008
#define RTC_BY_OSCIN_CTRL1			0x000c
#define RTC_CTRL				0x0010
#define SYS_CLK_CTRL0				0x0040
#define SYS_CLK_EN0_REG0			0x0044
#define SYS_CLK_EN0_REG1			0x0048
#define SYS_CLK_EN0_REG2			0x004c
#define AXI_CLK_CTRL0				0x006c
#define CLK12_24_CTRL				0x00a8
#define AXI_CLK_EN0				0x00ac
#define VDIN_MEAS_CLK_CTRL			0x00f8
#define VAPB_CLK_CTRL				0x00fc
#define MIPIDSI_PHY_CLK_CTRL			0x0104
#define GE2D_CLK_CTRL				0x010c
#define ISP0_CLK_CTRL				0x0110
#define DEWARPA_CLK_CTRL			0x0114
#define VOUTENC_CLK_CTRL			0x0118
#define VDEC_CLK_CTRL				0x0140
#define VDEC3_CLK_CTRL				0x0148
#define TS_CLK_CTRL				0x0158
#define ETH_CLK_CTRL				0x0164
#define NAND_CLK_CTRL				0x0168
#define SD_EMMC_CLK_CTRL			0x016c
#define SPICC_CLK_CTRL				0x0174
#define GEN_CLK_CTRL				0x0178
#define SAR_CLK_CTRL0				0x017c
#define PWM_CLK_AB_CTRL				0x0180
#define PWM_CLK_CD_CTRL				0x0184
#define PWM_CLK_EF_CTRL				0x0188
#define PWM_CLK_GH_CTRL				0x018c
#define PWM_CLK_IJ_CTRL				0x0190
#define PWM_CLK_KL_CTRL				0x0194
#define PWM_CLK_MN_CTRL				0x0198
#define VC9000E_CLK_CTRL			0x019c
#define SPIFC_CLK_CTRL				0x01a0
#define NNA_CLK_CTRL				0x0220

#endif  /* __AML_C3_PERIPHERALS_H__ */
