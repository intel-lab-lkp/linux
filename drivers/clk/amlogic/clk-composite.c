// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-composite.h"

/*
 * Amlogic composite clock module:
 *
 *             mux       div         gate
 *              |         |            |
 *         +----|---------|------------|------+
 *         |   \|/        |            |      |
 *         |   |\         |            |      |
 * clk0 ------>| |        |            |      |
 * clk1 ------>| |        |            |      |
 * clk2 ------>| |       \|/          \|/     |
 * clk3 ------>| |     +-----+     +------+   |
 *         |   | |---->| div |---->| gate |------> clk out
 * clk4 ------>| |     +-----+     +------+   |
 * clk5 ------>| |                            |
 * clk6 ------>| |                            |
 * clk7 ------>| |                            |
 *         |   |/                             |
 *         |                                  |
 *         +----------------------------------+
 *
 * Amlogic composite clocks support up to 8 clock sources, and the divider width
 * is configurable.
 *
 * The register bit-field allocation rules for mux, div, and gate are as
 * follows:
 * mux: bit[11:9] or bit[27:25]
 * div: bit[7:0] or bit[23:16]
 * gate: bit[8] or bit[24]
 */

#define CLK_COMPOSITE_MUX_SHIFT		9
#define CLK_COMPOSITE_MUX_MASK		0x7

#define CLK_COMPOSITE_DIV_SHIFT		0

#define CLK_COMPOSITE_GATE_SHIFT		8

static u8 aml_clk_composite_get_parent(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, composite->reg_offset, &val);
	if (ret)
		return ret;

	val >>=  CLK_COMPOSITE_MUX_SHIFT;
	val &= CLK_COMPOSITE_MUX_MASK;

	return clk_mux_val_to_index(hw, composite->table, 0, val);
}

static int aml_clk_composite_set_parent(struct clk_hw *hw, u8 index)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	unsigned int val = clk_mux_index_to_val(composite->table, 0, index);
	int mux_shift = composite->bit_offset + CLK_COMPOSITE_MUX_SHIFT;

	return regmap_update_bits(clk->map, composite->reg_offset,
				  CLK_COMPOSITE_MUX_MASK << mux_shift,
				  val << mux_shift);
}

static unsigned long aml_clk_composite_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, composite->reg_offset, &val);
	if (ret)
		/* Gives a hint that something is wrong */
		return 0;

	val >>= composite->bit_offset + CLK_COMPOSITE_DIV_SHIFT;
	val &= clk_div_mask(composite->div_width);

	return divider_recalc_rate(hw, parent_rate, val, NULL, 0,
				   composite->div_width);
}

static int
aml_clk_composite_determine_rate_for_parent(struct clk_hw *rate_hw,
					    struct clk_rate_request *req,
					    struct clk_hw *parent_hw)
{
	struct aml_clk *clk = to_aml_clk(rate_hw);
	struct aml_clk_composite_data *composite = clk->data;

	req->best_parent_hw = parent_hw;
	req->best_parent_rate = clk_hw_get_rate(parent_hw);

	return divider_determine_rate(rate_hw, req, NULL,
				      composite->div_width, 0);
}

static int aml_clk_composite_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	struct clk_hw *parent;
	struct clk_rate_request tmp_req;
	unsigned long rate_diff;
	unsigned long best_rate_diff = ULONG_MAX;
	unsigned long best_rate = 0;
	int i, ret;

	req->best_parent_hw = NULL;

	parent = clk_hw_get_parent(hw);
	clk_hw_forward_rate_request(hw, req, parent, &tmp_req, req->rate);
	ret = aml_clk_composite_determine_rate_for_parent(hw, &tmp_req,
							  parent);
	if (ret)
		return ret;

	/*
	 * Check if rate can be satisfied by current parent clock. Avoid parent
	 * switching when possible to reduce glitches.
	 */
	if (req->rate == tmp_req.rate ||
	    (clk_hw_get_flags(hw) & CLK_SET_RATE_NO_REPARENT)) {
		req->rate = tmp_req.rate;
		req->best_parent_hw = tmp_req.best_parent_hw;
		req->best_parent_rate = tmp_req.best_parent_rate;

		return 0;
	}

	for (i = 0; i < clk_hw_get_num_parents(hw); i++) {
		parent = clk_hw_get_parent_by_index(hw, i);
		if (!parent)
			continue;

		clk_hw_forward_rate_request(hw, req, parent, &tmp_req,
					    req->rate);
		ret = aml_clk_composite_determine_rate_for_parent(hw, &tmp_req,
								  parent);
		if (ret)
			continue;

		if (req->rate >= tmp_req.rate)
			rate_diff = req->rate - tmp_req.rate;
		else
			rate_diff = tmp_req.rate - req->rate;

		if (!rate_diff || !req->best_parent_hw ||
		    best_rate_diff > rate_diff) {
			req->best_parent_hw = parent;
			req->best_parent_rate = tmp_req.best_parent_rate;
			best_rate_diff = rate_diff;
			best_rate = tmp_req.rate;
		}

		if (!rate_diff)
			return 0;
	}

	req->rate = best_rate;
	return 0;
}

static int aml_clk_composite_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	unsigned int val;
	int ret;
	int div_shift = composite->bit_offset + CLK_COMPOSITE_DIV_SHIFT;

	ret = divider_get_val(rate, parent_rate, NULL, composite->div_width, 0);
	if (ret < 0)
		return ret;

	val = (unsigned int)ret << div_shift;
	return regmap_update_bits(clk->map, composite->reg_offset,
				  clk_div_mask(composite->div_width) <<
				  div_shift, val);
}

static int aml_clk_composite_set_rate_and_parent(struct clk_hw *hw,
						 unsigned long rate,
						 unsigned long parent_rate,
						 u8 index)
{
	unsigned long temp_rate;

	temp_rate = aml_clk_composite_recalc_rate(hw, parent_rate);
	if (temp_rate > rate) {
		aml_clk_composite_set_rate(hw, rate, parent_rate);
		aml_clk_composite_set_parent(hw, index);
	} else {
		aml_clk_composite_set_parent(hw, index);
		aml_clk_composite_set_rate(hw, rate, parent_rate);
	}

	return 0;
}

static int aml_clk_composite_is_enabled(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	unsigned int val;

	regmap_read(clk->map, composite->reg_offset, &val);
	val &= BIT(composite->bit_offset + CLK_COMPOSITE_GATE_SHIFT);

	return val ? 1 : 0;
}

static int aml_clk_composite_enable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	u8 bit_idx = composite->bit_offset + CLK_COMPOSITE_GATE_SHIFT;

	return regmap_update_bits(clk->map, composite->reg_offset,
				  BIT(bit_idx), BIT(bit_idx));
}

static void aml_clk_composite_disable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_composite_data *composite = clk->data;
	u8 bit_idx = composite->bit_offset + CLK_COMPOSITE_GATE_SHIFT;

	regmap_update_bits(clk->map, composite->reg_offset,
			   BIT(bit_idx), 0);
}

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static void aml_clk_composite_debug_init(struct clk_hw *hw,
					 struct dentry *dentry)
{
	debugfs_create_file("clk_type", 0444, dentry, hw, &aml_clk_type_fops);
	debugfs_create_file("clk_available_rates", 0444, dentry, hw,
			    &aml_clk_div_available_rates_fops);
}
#endif /* CONFIG_DEBUG_FS */

const struct clk_ops aml_clk_composite_ops = {
	.get_parent = aml_clk_composite_get_parent,
	.set_parent = aml_clk_composite_set_parent,
	.determine_rate = aml_clk_composite_determine_rate,
	.recalc_rate = aml_clk_composite_recalc_rate,
	.set_rate = aml_clk_composite_set_rate,
	.set_rate_and_parent = aml_clk_composite_set_rate_and_parent,
	.enable = aml_clk_composite_enable,
	.disable = aml_clk_composite_disable,
	.is_enabled = aml_clk_composite_is_enabled,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_composite_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_composite_ops, "CLK_AMLOGIC");

MODULE_DESCRIPTION("Amlogic Composite Clock Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
