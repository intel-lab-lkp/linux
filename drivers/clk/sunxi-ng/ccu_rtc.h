/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021 Samuel Holland <samuel@sholland.org>
 */

#ifndef _CCU_RTC_H_
#define _CCU_RTC_H_

#define IOSC_ACCURACY			300000000 /* 30% */
#define IOSC_RATE			16000000

#define LOSC_RATE			32768
#define LOSC_RATE_SHIFT			15

#define LOSC_CTRL_REG			0x0
#define LOSC_CTRL_KEY			0x16aa0000

#define IOSC_32K_CLK_DIV_REG		0x8
#define IOSC_32K_CLK_DIV		GENMASK(4, 0)
#define IOSC_32K_PRE_DIV		32

#define IOSC_CLK_CALI_REG		0xc
#define IOSC_CLK_CALI_DIV_ONES		22
#define IOSC_CLK_CALI_EN		BIT(1)
#define IOSC_CLK_CALI_SRC_SEL		BIT(0)

#define LOSC_OUT_GATING_REG		0x60

#define DCXO_CTRL_REG			0x160
#define DCXO_CTRL_DCXO_EN		BIT(1)
#define DCXO_CTRL_CLK16M_RC_EN		BIT(0)

#define DCXO_GATING_REG			0x16c
#define DCXO_SERDES1_GATING		BIT(5)
#define DCXO_SERDES0_GATING		BIT(4)
#define DCXO_HDMI_GATING		BIT(1)
#define DCXO_UFS_GATING			BIT(0)

#define SUN6I_RTC_AUX_ID(_name)		"rtc_sun6i." #_name

extern const struct clk_ops ccu_iosc_ops;
extern const struct clk_ops ccu_iosc_32k_ops;

#endif /* _CCU_RTC_H_ */
