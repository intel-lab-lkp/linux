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
#include <linux/io.h>
#include <linux/types.h>

struct clk;

#define TTC_DEFAULT_TIMER_WIDTH		16

/* Per-channel register offset stride */
#define TTC_CH_OFFSET			0x04

#define TTC_NUM_CHANNELS		3

/* Timer Register Offset Definitions of Timer 1 */
#define TTC_CLK_CNTRL_OFFSET		0x00 /* Clock Control Reg, RW */
#define TTC_CNT_CNTRL_OFFSET		0x0C /* Counter Control Reg, RW */
#define TTC_COUNT_VAL_OFFSET		0x18 /* Counter Value Reg, RO */
#define TTC_INTR_VAL_OFFSET		0x24 /* Interval Count Reg, RW */
#define TTC_MATCH_CNT_VAL_OFFSET	0x30 /* Match 1 Count Reg, RW */
#define TTC_ISR_OFFSET			0x54 /* Interrupt Status Reg, RO */
#define TTC_IER_OFFSET			0x60 /* Interrupt Enable Reg, RW */

#define TTC_CNT_CNTRL_DISABLE_MASK	BIT(0)
#define TTC_CNT_CNTRL_INT		BIT(1)
#define TTC_CNT_CNTRL_MATCH		BIT(3)
#define TTC_CNT_CNTRL_RST		BIT(4)
#define TTC_CNT_CNTRL_WAVE_EN		BIT(5)
#define TTC_CNT_CNTRL_WAVE_POL		BIT(6)

#define TTC_CLK_CNTRL_PS_EN		BIT(0)
#define TTC_CLK_CNTRL_CSRC_MASK		BIT(5) /* clock source */
#define TTC_CLK_CNTRL_PSV_MASK		GENMASK(4, 1)
#define TTC_CLK_CNTRL_PSV_SHIFT		1
#define TTC_CLK_CNTRL_PSV_MAX		15 /* 4-bit prescaler field */

/**
 * struct ttc_shared_data - Data passed from parent to auxiliary device
 * @base:		MMIO base address
 * @clk:		Input clock
 * @rate:		Input clock rate in Hz
 * @timer_width:	Counter width in bits (16 or 32)
 */
struct ttc_shared_data {
	void __iomem		*base;
	struct clk		*clk;
	unsigned long		rate;
	u32			timer_width;
};

static inline u32 ttc_ch_readl(const struct ttc_shared_data *sd,
			       unsigned int ch, u32 reg)
{
	return readl_relaxed(sd->base + reg + TTC_CH_OFFSET * ch);
}

static inline void ttc_ch_writel(const struct ttc_shared_data *sd,
				 unsigned int ch, u32 reg, u32 val)
{
	writel_relaxed(val, sd->base + reg + TTC_CH_OFFSET * ch);
}

#endif /* _LINUX_TIMER_CADENCE_TTC_H */
