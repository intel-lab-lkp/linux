// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/clk-provider.h>
#include <linux/export.h>
#include <linux/regmap.h>
#include "clk-regmap-mux.h"

static u8 rtk_clk_regmap_mux_get_parent(struct clk_hw *hw)
{
	struct rtk_clk_regmap_mux *clkm = to_rtk_clk_regmap_mux(hw);
	u32 val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->mux_ofs, &val);
	if (ret)
		return 0xff;

	return (val >> clkm->shift) & clkm->mask;
}

static int rtk_clk_regmap_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct rtk_clk_regmap_mux *clkm = to_rtk_clk_regmap_mux(hw);

	return regmap_update_bits(clkm->clkr.regmap, clkm->mux_ofs,
				  clkm->mask << clkm->shift, (u32)index << clkm->shift);
}

const struct clk_ops rtk_clk_regmap_mux_ops = {
	.set_parent = rtk_clk_regmap_mux_set_parent,
	.get_parent = rtk_clk_regmap_mux_get_parent,
	.determine_rate = __clk_mux_determine_rate,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_regmap_mux_ops, "CLK_REALTEK");
