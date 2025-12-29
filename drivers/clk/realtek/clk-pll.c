// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include "clk-pll.h"

#define TIMEOUT 2000

static int wait_freq_ready(struct clk_pll *clkp)
{
	u32 pollval;

	if (!clkp->freq_ready_valid)
		return 0;

	return regmap_read_poll_timeout_atomic(clkp->clkr.regmap, clkp->freq_ready_reg, pollval,
					(pollval & clkp->freq_ready_mask) == clkp->freq_ready_val,
					0, TIMEOUT);
}

static bool is_power_on(struct clk_pll *clkp)
{
	u32 val;
	int ret;

	if (!clkp->power_reg)
		return true;

	ret = regmap_read(clkp->clkr.regmap, clkp->power_reg, &val);
	if (ret)
		return true;

	return (val & clkp->power_mask) == clkp->power_val_on;
}

static int get_freq_val_raw(struct clk_pll *clkp, u32 *val)
{
	int ret;

	ret = regmap_read(clkp->clkr.regmap, clkp->freq_reg, val);
	if (ret)
		return ret;

	return 0;
}

static int get_freq_val(struct clk_pll *clkp, u32 *val)
{
	int ret;
	u32 raw_val;

	ret = get_freq_val_raw(clkp, &raw_val);
	if (ret)
		return ret;

	*val = raw_val & clkp->freq_mask;
	return 0;
}

static void clk_pll_disable(struct clk_hw *hw)
{
	struct clk_pll *clkp = to_clk_pll(hw);

	if (!clkp->seq_power_off)
		return;

	regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_power_off,
			       clkp->num_seq_power_off);
}

static void clk_pll_disable_unused(struct clk_hw *hw)
{
	clk_pll_disable(hw);
}

static int clk_pll_is_enabled(struct clk_hw *hw)
{
	struct clk_pll *clkp = to_clk_pll(hw);

	return is_power_on(clkp);
}

static int clk_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct clk_pll *clkp = to_clk_pll(hw);
	const struct freq_table *ftblv = NULL;

	ftblv = ftbl_find_by_rate(clkp->freq_tbl, req->rate);
	return ftblv ? ftblv->rate : 0;
}

static unsigned long clk_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct clk_pll *clkp = to_clk_pll(hw);
	const struct freq_table *fv;
	u32 freq_val;
	int ret;

	ret = get_freq_val(clkp, &freq_val);
	if (ret)
		return 0;

	fv = ftbl_find_by_val_with_mask(clkp->freq_tbl, clkp->freq_mask,
					freq_val);
	return fv ? fv->rate : 0;
}

static int clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct clk_pll *clkp = to_clk_pll(hw);
	const struct freq_table *fv;
	u32 freq_val;
	int ret;

	fv = ftbl_find_by_rate(clkp->freq_tbl, rate);
	if (!fv || fv->rate != rate)
		return -EINVAL;

	ret = get_freq_val_raw(clkp, &freq_val);
	if (ret)
		return ret;

	freq_val &= ~clkp->freq_mask;
	freq_val |= (fv->val & clkp->freq_mask);

	if (clkp->seq_pre_set_freq) {
		ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_pre_set_freq,
					     clkp->num_seq_pre_set_freq);
		if (ret)
			return ret;
	}

	ret = regmap_write(clkp->clkr.regmap, clkp->freq_reg, freq_val);
	if (ret)
		return ret;

	if (clkp->seq_post_set_freq) {
		ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_post_set_freq,
					     clkp->num_seq_post_set_freq);
		if (ret)
			return ret;
	}

	if (is_power_on(clkp)) {
		ret = wait_freq_ready(clkp);
		if (ret)
			return ret;
	}

	return 0;
}

static int clk_pll_enable(struct clk_hw *hw)
{
	struct clk_pll *clkp = to_clk_pll(hw);
	int ret;

	if (!clkp->seq_power_on)
		return 0;

	if (is_power_on(clkp))
		return 0;

	ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_power_on,
				     clkp->num_seq_power_on);
	if (ret)
		return ret;

	return wait_freq_ready(clkp);
}

const struct clk_ops clk_pll_ops = {
	.enable         = clk_pll_enable,
	.disable        = clk_pll_disable,
	.is_enabled     = clk_pll_is_enabled,
	.disable_unused = clk_pll_disable_unused,
	.recalc_rate    = clk_pll_recalc_rate,
	.determine_rate = clk_pll_determine_rate,
	.set_rate       = clk_pll_set_rate,
};
EXPORT_SYMBOL_GPL(clk_pll_ops);

const struct clk_ops clk_pll_ro_ops = {
	.recalc_rate = clk_pll_recalc_rate,
};
EXPORT_SYMBOL_GPL(clk_pll_ro_ops);
