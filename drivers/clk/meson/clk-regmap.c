// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 BayLibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

#include <linux/device.h>
#include <linux/module.h>
#include "clk-regmap.h"

static void clk_regmap_devres_release(struct device *dev, void *res)
{
	/*
	 * Nothing to be done here
	 * This is meant as an id for devres related functions
	 */
}

static int clk_regmap_devres_match(struct device *dev, void *res, void *data)
{
	struct regmap **r = res;

	if (!r || !*r) {
		WARN_ON(!r || !*r);
		return 0;
	}

	/*
	 * NOTE:
	 * At the moment, only one regmap is needed so only one is expected
	 * to be registered. Should more be needed in the future,
	 * struct clk_regmap should evolve to carry the name of regmap it
	 * needs
	 */
	return 1;
}

int clk_regmap_init_regmap(struct device *dev, struct regmap *map)
{
	struct regmap **m;

	m = devres_alloc(clk_regmap_devres_release, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	*m = map;
	devres_add(dev, m);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(clk_regmap_init_regmap, CLK_MESON);

static struct regmap *clk_regmap_init_get_regmap(struct device *dev)
{
	struct regmap **r = devres_find(dev, clk_regmap_devres_release,
					clk_regmap_devres_match, NULL);

	if (!r)
		return NULL;

	return *r;
}

int clk_regmap_init(struct clk_hw *hw)
{
	struct device *dev = clk_hw_get_dev(hw);
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct regmap *map;

	/*
	 * Do not try to go through devres if there is not device associated
	 * with the clock. It may happen with early controller that have no
	 * device support. In this case, regmap should already be set.
	 */
	if (!dev)
		return clk->map ? 0 : -EINVAL;

	map = clk_regmap_init_get_regmap(dev);
	if (!map)
		return -EINVAL;

	/*
	 * Cache the regmap here so it not necessary to through devres
	 * for each clock action
	 */
	clk->map = map;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(clk_regmap_init, CLK_MESON);

static int clk_regmap_gate_endisable(struct clk_hw *hw, int enable)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_gate_data *gate = clk_get_regmap_gate_data(clk);
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;

	set ^= enable;

	return regmap_update_bits(clk->map, gate->offset, BIT(gate->bit_idx),
				  set ? BIT(gate->bit_idx) : 0);
}

static int clk_regmap_gate_enable(struct clk_hw *hw)
{
	return clk_regmap_gate_endisable(hw, 1);
}

static void clk_regmap_gate_disable(struct clk_hw *hw)
{
	clk_regmap_gate_endisable(hw, 0);
}

static int clk_regmap_gate_is_enabled(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_gate_data *gate = clk_get_regmap_gate_data(clk);
	unsigned int val;

	regmap_read(clk->map, gate->offset, &val);
	if (gate->flags & CLK_GATE_SET_TO_DISABLE)
		val ^= BIT(gate->bit_idx);

	val &= BIT(gate->bit_idx);

	return val ? 1 : 0;
}

const struct clk_ops clk_regmap_gate_ops = {
	.init		= clk_regmap_init,
	.enable = clk_regmap_gate_enable,
	.disable = clk_regmap_gate_disable,
	.is_enabled = clk_regmap_gate_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_gate_ops, CLK_MESON);

const struct clk_ops clk_regmap_gate_ro_ops = {
	.init		= clk_regmap_init,
	.is_enabled = clk_regmap_gate_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_gate_ro_ops, CLK_MESON);

static unsigned long clk_regmap_div_recalc_rate(struct clk_hw *hw,
						unsigned long prate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_div_data *div = clk_get_regmap_div_data(clk);
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, div->offset, &val);
	if (ret)
		/* Gives a hint that something is wrong */
		return 0;

	val >>= div->shift;
	val &= clk_div_mask(div->width);
	return divider_recalc_rate(hw, prate, val, div->table, div->flags,
				   div->width);
}

static int clk_regmap_div_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_div_data *div = clk_get_regmap_div_data(clk);
	unsigned int val;
	int ret;

	/* if read only, just return current value */
	if (div->flags & CLK_DIVIDER_READ_ONLY) {
		ret = regmap_read(clk->map, div->offset, &val);
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

static int clk_regmap_div_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_div_data *div = clk_get_regmap_div_data(clk);
	unsigned int val;
	int ret;

	ret = divider_get_val(rate, parent_rate, div->table, div->width,
			      div->flags);
	if (ret < 0)
		return ret;

	val = (unsigned int)ret << div->shift;
	return regmap_update_bits(clk->map, div->offset,
				  clk_div_mask(div->width) << div->shift, val);
};

/* Would prefer clk_regmap_div_ro_ops but clashes with qcom */

const struct clk_ops clk_regmap_divider_ops = {
	.init = clk_regmap_init,
	.recalc_rate = clk_regmap_div_recalc_rate,
	.determine_rate = clk_regmap_div_determine_rate,
	.set_rate = clk_regmap_div_set_rate,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_divider_ops, CLK_MESON);

const struct clk_ops clk_regmap_divider_ro_ops = {
	.init = clk_regmap_init,
	.recalc_rate = clk_regmap_div_recalc_rate,
	.determine_rate = clk_regmap_div_determine_rate,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_divider_ro_ops, CLK_MESON);

static u8 clk_regmap_mux_get_parent(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_mux_data *mux = clk_get_regmap_mux_data(clk);
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, mux->offset, &val);
	if (ret)
		return ret;

	val >>= mux->shift;
	val &= mux->mask;
	return clk_mux_val_to_index(hw, mux->table, mux->flags, val);
}

static int clk_regmap_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_mux_data *mux = clk_get_regmap_mux_data(clk);
	unsigned int val = clk_mux_index_to_val(mux->table, mux->flags, index);

	return regmap_update_bits(clk->map, mux->offset,
				  mux->mask << mux->shift,
				  val << mux->shift);
}

static int clk_regmap_mux_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct clk_regmap_mux_data *mux = clk_get_regmap_mux_data(clk);

	return clk_mux_determine_rate_flags(hw, req, mux->flags);
}

const struct clk_ops clk_regmap_mux_ops = {
	.init		= clk_regmap_init,
	.get_parent = clk_regmap_mux_get_parent,
	.set_parent = clk_regmap_mux_set_parent,
	.determine_rate = clk_regmap_mux_determine_rate,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_mux_ops, CLK_MESON);

const struct clk_ops clk_regmap_mux_ro_ops = {
	.init		= clk_regmap_init,
	.get_parent = clk_regmap_mux_get_parent,
};
EXPORT_SYMBOL_NS_GPL(clk_regmap_mux_ro_ops, CLK_MESON);

MODULE_DESCRIPTION("Amlogic regmap backed clock driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(CLK_MESON);
