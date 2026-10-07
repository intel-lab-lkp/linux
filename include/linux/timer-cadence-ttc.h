/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared definitions for the Cadence Triple Timer Counter (TTC).
 *
 * Copyright (C) 2011 - 2013, Xilinx, Inc.
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _LINUX_TIMER_CADENCE_TTC_H
#define _LINUX_TIMER_CADENCE_TTC_H

#include <linux/bits.h>

#define TTC_DEFAULT_TIMER_WIDTH		16

/* Per-channel register offset stride */
#define TTC_CH_OFFSET			0x04

/* Timer Register Offset Definitions of Timer 1 */
#define TTC_CLK_CNTRL_OFFSET		0x00 /* Clock Control Reg, RW */
#define TTC_CNT_CNTRL_OFFSET		0x0C /* Counter Control Reg, RW */
#define TTC_COUNT_VAL_OFFSET		0x18 /* Counter Value Reg, RO */
#define TTC_INTR_VAL_OFFSET		0x24 /* Interval Count Reg, RW */
#define TTC_ISR_OFFSET			0x54 /* Interrupt Status Reg, RO */
#define TTC_IER_OFFSET			0x60 /* Interrupt Enable Reg, RW */

#define TTC_CNT_CNTRL_DISABLE_MASK	BIT(0)

#define TTC_CLK_CNTRL_CSRC_MASK		BIT(5) /* clock source */
#define TTC_CLK_CNTRL_PSV_MASK		GENMASK(4, 1)
#define TTC_CLK_CNTRL_PSV_SHIFT		1

#endif /* _LINUX_TIMER_CADENCE_TTC_H */
