// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-basic.h"

/*
 * This file implements the ops functions for basic Amlogic clock models
 * (mux/div/gate), based on clk-mux.c, clk-divider.c, and clk-gate.c in the CCF.
 */

static int aml_clk_gate_endisable(struct clk_hw *hw, int enable)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_gate_data *gate = clk->data;
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;

	set ^= enable;

	return regmap_update_bits(clk->map, gate->reg_offset,
				  BIT(gate->bit_idx),
				  set ? BIT(gate->bit_idx) : 0);
}

static int aml_clk_gate_enable(struct clk_hw *hw)
{
	return aml_clk_gate_endisable(hw, 1);
}

static void aml_clk_gate_disable(struct clk_hw *hw)
{
	aml_clk_gate_endisable(hw, 0);
}

static int aml_clk_gate_is_enabled(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_gate_data *gate = clk->data;
	unsigned int val;

	regmap_read(clk->map, gate->reg_offset, &val);
	if (gate->flags & CLK_GATE_SET_TO_DISABLE)
		val ^= BIT(gate->bit_idx);

	val &= BIT(gate->bit_idx);

	return val ? 1 : 0;
}

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static void aml_clk_basic_debug_init(struct clk_hw *hw, struct dentry *dentry)
{
	struct aml_clk *clk = to_aml_clk(hw);

	debugfs_create_file("clk_type", 0444, dentry, hw, &aml_clk_type_fops);
	if (clk->type == AML_CLKTYPE_DIV)
		debugfs_create_file("clk_available_rates", 0444, dentry, hw,
				    &aml_clk_div_available_rates_fops);
}
#endif /* CONFIG_DEBUG_FS */

const struct clk_ops aml_clk_gate_ops = {
	.enable = aml_clk_gate_enable,
	.disable = aml_clk_gate_disable,
	.is_enabled = aml_clk_gate_is_enabled,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_basic_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_gate_ops, "CLK_AMLOGIC");

static unsigned long aml_clk_div_recalc_rate(struct clk_hw *hw,
					     unsigned long prate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_divider_data *div = clk->data;
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, div->reg_offset, &val);
	if (ret)
		/* Gives a hint that something is wrong */
		return 0;

	val >>= div->shift;
	val &= clk_div_mask(div->width);

	return divider_recalc_rate(hw, prate, val, div->table, div->flags,
				   div->width);
}

static int aml_clk_div_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_divider_data *div = clk->data;
	unsigned int val;
	int ret;

	/* if read only, just return current value */
	if (div->flags & CLK_DIVIDER_READ_ONLY) {
		ret = regmap_read(clk->map, div->reg_offset, &val);
		if (ret)
			return ret;

		val >>= div->shift;
		val &= clk_div_mask(div->width);

		return divider_ro_determine_rate(hw, req, div->table,
						 div->width, div->flags, val);
	}

	return divider_determine_rate(hw, req, div->table, div->width,
				      div->flags);
}

static int aml_clk_div_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_divider_data *div = clk->data;
	unsigned int val;
	int ret;

	ret = divider_get_val(rate, parent_rate, div->table, div->width,
			      div->flags);
	if (ret < 0)
		return ret;

	val = (unsigned int)ret << div->shift;

	return regmap_update_bits(clk->map, div->reg_offset,
				  clk_div_mask(div->width) << div->shift, val);
};

const struct clk_ops aml_clk_divider_ops = {
	.recalc_rate = aml_clk_div_recalc_rate,
	.determine_rate = aml_clk_div_determine_rate,
	.set_rate = aml_clk_div_set_rate,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_basic_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_divider_ops, "CLK_AMLOGIC");

const struct clk_ops aml_clk_divider_ro_ops = {
	.recalc_rate = aml_clk_div_recalc_rate,
	.determine_rate = aml_clk_div_determine_rate,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_basic_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_divider_ro_ops, "CLK_AMLOGIC");

static u8 aml_clk_mux_get_parent(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_mux_data *mux = clk->data;
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, mux->reg_offset, &val);
	if (ret)
		return ret;

	val >>= mux->shift;
	val &= mux->mask;
	return clk_mux_val_to_index(hw, mux->table, mux->flags, val);
}

static int aml_clk_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_mux_data *mux = clk->data;
	unsigned int val = clk_mux_index_to_val(mux->table, mux->flags, index);

	return regmap_update_bits(clk->map, mux->reg_offset,
				  mux->mask << mux->shift,
				  val << mux->shift);
}

static int aml_clk_mux_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_mux_data *mux = clk->data;

	return clk_mux_determine_rate_flags(hw, req, mux->flags);
}

const struct clk_ops aml_clk_mux_ops = {
	.get_parent = aml_clk_mux_get_parent,
	.set_parent = aml_clk_mux_set_parent,
	.determine_rate = aml_clk_mux_determine_rate,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_basic_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_mux_ops, "CLK_AMLOGIC");

const struct clk_ops aml_clk_mux_ro_ops = {
	.get_parent = aml_clk_mux_get_parent,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_basic_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_mux_ro_ops, "CLK_AMLOGIC");

MODULE_DESCRIPTION("Amlogic Basic Clock Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
