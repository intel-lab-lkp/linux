/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * Authors:
 *	Yifeng Huang <huangyifeng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 */

#ifndef __ESWIN_CLK_H__
#define __ESWIN_CLK_H__

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#define CLK_FREQ_1800M 1800000000
#define CLK_FREQ_1700M 1700000000
#define CLK_FREQ_1600M 1600000000
#define CLK_FREQ_1500M 1500000000
#define CLK_FREQ_1400M 1400000000
#define CLK_FREQ_1300M 1300000000
#define CLK_FREQ_1200M 1200000000
#define CLK_FREQ_1000M 1000000000
#define CLK_FREQ_900M 900000000
#define CLK_FREQ_800M 800000000
#define CLK_FREQ_700M 700000000
#define CLK_FREQ_600M 600000000
#define CLK_FREQ_500M 500000000
#define CLK_FREQ_400M 400000000
#define CLK_FREQ_200M 200000000
#define CLK_FREQ_100M 100000000
#define CLK_FREQ_24M 24000000

#define APLL_HIGH_FREQ 983040000
#define APLL_LOW_FREQ 225792000

struct eswin_clk_pll {
	struct clk_hw hw;
	void __iomem *ctrl_reg0;
	u8 pllen_shift;
	u8 pllen_width;
	u8 refdiv_shift;
	u8 refdiv_width;
	u8 fbdiv_shift;
	u8 fbdiv_width;

	void __iomem *ctrl_reg1;
	u8 frac_shift;
	u8 frac_width;

	void __iomem *ctrl_reg2;
	u8 postdiv1_shift;
	u8 postdiv1_width;
	u8 postdiv2_shift;
	u8 postdiv2_width;

	void __iomem *status_reg;
	u8 lock_shift;
	u8 lock_width;
};

void __init eswin_clk_gate_register(struct device_node *np);
void __init eswin_clk_mux_register(struct device_node *np);
void __init eswin_clk_div_register(struct device_node *np);
void __init eswin_clk_pll_register(struct device_node *np);

#endif /* __ESWIN_CLK_H__ */
