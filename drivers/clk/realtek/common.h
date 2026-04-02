/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2016-2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_COMMON_H
#define __CLK_REALTEK_COMMON_H

#include <linux/clk-provider.h>

#define __clk_regmap_hw(_p) ((_p)->hw)

struct device;
struct platform_device;
struct regmap;

struct clk_regmap {
	struct clk_hw hw;
	struct regmap *regmap;
};

struct rtk_clk_desc {
	struct clk_hw_onecell_data *clk_data;
	struct clk_regmap **clks;
	size_t num_clks;
};

static inline struct clk_regmap *to_clk_regmap(struct clk_hw *hw)
{
	return container_of(hw, struct clk_regmap, hw);
}

int rtk_clk_probe(struct platform_device *pdev, const struct rtk_clk_desc *desc,
		  const char *aux_name);

#endif /* __CLK_REALTEK_COMMON_H */
