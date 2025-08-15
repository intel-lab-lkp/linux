// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * ESWIN EIC7700 CLK Provider Driver
 *
 * Authors:
 *	Yifeng Huang <huangyifeng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include "clk.h"

static void __init eic7700_clk_pll_init(struct device_node *np)
{
	eswin_clk_pll_register(np);
}

static void __init eic7700_clk_mux_init(struct device_node *np)
{
	eswin_clk_mux_register(np);
}

static void __init eic7700_clk_div_init(struct device_node *np)
{
	eswin_clk_div_register(np);
}

static void __init eic7700_clk_gate_init(struct device_node *np)
{
	eswin_clk_gate_register(np);
}

CLK_OF_DECLARE(eic7700_clk_pll, "eswin,pll-clock", eic7700_clk_pll_init);
CLK_OF_DECLARE(eic7700_clk_mux, "eswin,mux-clock", eic7700_clk_mux_init);
CLK_OF_DECLARE(eic7700_clk_div, "eswin,divider-clock", eic7700_clk_div_init);
CLK_OF_DECLARE(eic7700_clk_gate, "eswin,gate-clock", eic7700_clk_gate_init);
