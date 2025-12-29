/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2016-2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_COMMON_H
#define __CLK_REALTEK_COMMON_H

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include "reset.h"

struct device;
struct platform_device;

struct clk_regmap {
	struct clk_hw hw;
	struct regmap *regmap;
};

#define to_clk_regmap(_hw) container_of(_hw, struct clk_regmap, hw)
#define __clk_regmap_hw(_p) ((_p)->hw)

struct rtk_clk_desc {
	struct clk_hw_onecell_data *clk_data;
	struct clk_regmap **clks;
	size_t num_clks;
	struct rtk_reset_bank *reset_banks;
	size_t num_reset_banks;
};

int rtk_clk_probe(struct platform_device *pdev,
		  const struct rtk_clk_desc *desc);

#endif /* __CLK_REALTEK_COMMON_H */
