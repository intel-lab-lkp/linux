/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved
 */

#ifndef _RTC_NVIDIA_VRS_H_
#define _RTC_NVIDIA_VRS_H_

#include <linux/types.h>

/* Vendor Info */
#define NVVRS_REG_VENDOR_ID			0x00
#define NVVRS_REG_MODEL_REV			0x01

/*  Interrupts registers */
#define NVVRS_REG_INT_SRC1			0x10
#define NVVRS_REG_INT_SRC2			0x11
#define NVVRS_REG_INT_VENDOR			0x12

/* Control Registers */
#define NVVRS_REG_CTL_1				0x28
#define NVVRS_REG_CTL_2				0x29

/* RTC Registers */
#define NVVRS_REG_RTC_T3			0x70
#define NVVRS_REG_RTC_T2			0x71
#define NVVRS_REG_RTC_T1			0x72
#define NVVRS_REG_RTC_T0			0x73
#define NVVRS_REG_RTC_A3			0x74
#define NVVRS_REG_RTC_A2			0x75
#define NVVRS_REG_RTC_A1			0x76
#define NVVRS_REG_RTC_A0			0x77

/* Interrupt Mask */
#define NVVRS_INT_SRC1_RSTIRQ_MASK		BIT(0)
#define NVVRS_INT_SRC1_OSC_MASK			BIT(1)
#define NVVRS_INT_SRC1_EN_MASK			BIT(2)
#define NVVRS_INT_SRC1_RTC_MASK			BIT(3)
#define NVVRS_INT_SRC1_PEC_MASK			BIT(4)
#define NVVRS_INT_SRC1_WDT_MASK			BIT(5)
#define NVVRS_INT_SRC1_EM_PD_MASK		BIT(6)
#define NVVRS_INT_SRC1_INTERNAL_MASK		BIT(7)
#define NVVRS_INT_SRC2_PBSP_MASK		BIT(0)
#define NVVRS_INT_SRC2_ECC_DED_MASK		BIT(1)
#define NVVRS_INT_SRC2_TSD_MASK			BIT(2)
#define NVVRS_INT_SRC2_LDO_MASK			BIT(3)
#define NVVRS_INT_SRC2_BIST_MASK		BIT(4)
#define NVVRS_INT_SRC2_RT_CRC_MASK		BIT(5)
#define NVVRS_INT_SRC2_VENDOR_MASK		BIT(7)
#define NVVRS_INT_VENDOR0_MASK			BIT(0)
#define NVVRS_INT_VENDOR1_MASK			BIT(1)
#define NVVRS_INT_VENDOR2_MASK			BIT(2)
#define NVVRS_INT_VENDOR3_MASK			BIT(3)
#define NVVRS_INT_VENDOR4_MASK			BIT(4)
#define NVVRS_INT_VENDOR5_MASK			BIT(5)
#define NVVRS_INT_VENDOR6_MASK			BIT(6)
#define NVVRS_INT_VENDOR7_MASK			BIT(7)

/* Controller Register Mask */
#define NVVRS_REG_CTL_1_FORCE_SHDN		(BIT(0) | BIT(1))
#define NVVRS_REG_CTL_1_FORCE_ACT		BIT(2)
#define NVVRS_REG_CTL_1_FORCE_INT		BIT(3)
#define NVVRS_REG_CTL_2_EN_PEC			BIT(0)
#define NVVRS_REG_CTL_2_REQ_PEC			BIT(1)
#define NVVRS_REG_CTL_2_RTC_PU			BIT(2)
#define NVVRS_REG_CTL_2_RTC_WAKE		BIT(3)
#define NVVRS_REG_CTL_2_RST_DLY			0xF0

enum nvvrs_irq_regs {
	NVVRS_IRQ_REG_INT_SRC1 = 0,
	NVVRS_IRQ_REG_INT_SRC2 = 1,
	NVVRS_IRQ_REG_INT_VENDOR = 2,
	NVVRS_IRQ_REG_COUNT = 3,
};

#endif /* _RTC_NVIDIA_VRS_H_ */

