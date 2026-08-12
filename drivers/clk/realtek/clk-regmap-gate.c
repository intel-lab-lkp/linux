// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/export.h>
#include <linux/regmap.h>
#include "clk-regmap-gate.h"

static int rtk_clk_regmap_gate_enable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_gate *clkg = to_rtk_clk_regmap_gate(hw);
	unsigned int mask;
	unsigned int val;

	mask = BIT(clkg->bit_idx);
	val = BIT(clkg->bit_idx);

	if (clkg->write_en) {
		mask |= BIT(clkg->bit_idx + 1);
		val |= BIT(clkg->bit_idx + 1);
	}

	return regmap_update_bits(clkg->clkr.regmap, clkg->gate_ofs, mask, val);
}

static void rtk_clk_regmap_gate_disable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_gate *clkg = to_rtk_clk_regmap_gate(hw);
	unsigned int mask;
	unsigned int val;

	mask = BIT(clkg->bit_idx);
	val = 0;

	if (clkg->write_en) {
		mask |= BIT(clkg->bit_idx + 1);
		val |= BIT(clkg->bit_idx + 1);
	}

	regmap_update_bits(clkg->clkr.regmap, clkg->gate_ofs, mask, val);
}

static int rtk_clk_regmap_gate_is_enabled(struct clk_hw *hw)
{
	struct rtk_clk_regmap_gate *clkg = to_rtk_clk_regmap_gate(hw);
	int ret;
	u32 val;

	ret = regmap_read(clkg->clkr.regmap, clkg->gate_ofs, &val);
	if (ret < 0)
		return ret;

	return !!(val & BIT(clkg->bit_idx));
}

const struct clk_ops rtk_clk_regmap_gate_ops = {
	.enable     = rtk_clk_regmap_gate_enable,
	.disable    = rtk_clk_regmap_gate_disable,
	.is_enabled = rtk_clk_regmap_gate_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_regmap_gate_ops, "CLK_REALTEK");

const struct clk_ops rtk_clk_regmap_gate_ro_ops = {
	.is_enabled = rtk_clk_regmap_gate_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_regmap_gate_ro_ops, "CLK_REALTEK");
