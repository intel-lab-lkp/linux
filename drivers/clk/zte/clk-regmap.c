// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2018 BayLibre, SAS.
 * Copyright (c) 2026 Stefan Dösinger.
 * Author: Stefan Dösinger <stefandoesinger@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/bits.h>
#include <linux/err.h>

#include "clk-zx.h"

struct zte_clk_regmap {
	struct clk_hw	hw;
	struct regmap	*map;
	u16		reg;
	u8		shift;
	u8		size;
};

static inline struct zte_clk_regmap *to_zte_clk_regmap(struct clk_hw *hw)
{
	return container_of(hw, struct zte_clk_regmap, hw);
}

static int zte_clk_regmap_gate_enable(struct clk_hw *hw)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);

	return regmap_set_bits(clk->map, clk->reg, BIT(clk->shift));
}

static void zte_clk_regmap_gate_disable(struct clk_hw *hw)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);

	regmap_clear_bits(clk->map, clk->reg, BIT(clk->shift));
}

static int zte_clk_regmap_gate_is_enabled(struct clk_hw *hw)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);

	return regmap_test_bits(clk->map, clk->reg, BIT(clk->shift));
}

static const struct clk_ops zte_clk_regmap_gate_ops = {
	.enable		= zte_clk_regmap_gate_enable,
	.disable	= zte_clk_regmap_gate_disable,
	.is_enabled	= zte_clk_regmap_gate_is_enabled,
};

int zx_clk_register_gates(struct device *dev, struct regmap *regmap,
			  const struct zx_gate_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks)
{
	struct zte_clk_regmap *clk;
	unsigned int i;
	int res;

	for (i = 0; i < num; ++i) {
		struct clk_init_data init = {};

		clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
		if (!clk)
			return -ENOMEM;

		init.name = desc[i].name;
		init.ops = &zte_clk_regmap_gate_ops;
		init.parent_names = &desc[i].parent;
		init.num_parents = 1;
		init.flags = CLK_SET_RATE_PARENT | desc[i].flags;
		clk->hw.init = &init;
		clk->map = regmap;
		clk->reg = desc[i].reg;
		clk->shift = desc[i].shift;
		clk->size = 1;

		res = devm_clk_hw_register(dev, &clk->hw);
		if (res)
			return dev_err_probe(dev, res, "Failed to register clk %s\n", desc[i].name);

		if (desc[i].id)
			clocks->hws[desc[i].id] = &clk->hw;
	}

	return 0;
}

static unsigned long zte_clk_regmap_div_recalc_rate(struct clk_hw *hw,
						unsigned long prate)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, clk->reg, &val);
	if (ret)
		/* Gives a hint that something is wrong */
		return 0;

	val >>= clk->shift;
	val &= clk_div_mask(clk->size);
	return divider_recalc_rate(hw, prate, val, NULL, 0, clk->size);
}

static int zte_clk_regmap_div_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);

	return divider_determine_rate(hw, req, NULL, clk->size, 0);
}

static int zte_clk_regmap_div_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);
	unsigned int val;
	int ret;

	ret = divider_get_val(rate, parent_rate, NULL, clk->size, 0);
	if (ret < 0)
		return ret;

	val = (unsigned int)ret << clk->shift;
	return regmap_update_bits(clk->map, clk->reg, clk_div_mask(clk->size) << clk->shift, val);
}

static const struct clk_ops zte_clk_regmap_divider_ops = {
	.recalc_rate = zte_clk_regmap_div_recalc_rate,
	.determine_rate = zte_clk_regmap_div_determine_rate,
	.set_rate = zte_clk_regmap_div_set_rate,
};

int zx_clk_register_dividers(struct device *dev, struct regmap *regmap,
			     const struct zx_div_desc *desc, unsigned int num)
{
	struct zte_clk_regmap *clk;
	unsigned int i;
	int res;

	for (i = 0; i < num; ++i) {
		struct clk_init_data init = {};

		clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
		if (!clk)
			return -ENOMEM;

		init.name = desc[i].name;
		init.ops = &zte_clk_regmap_divider_ops;
		init.parent_names = &desc[i].parent;
		init.num_parents = 1;
		init.flags = CLK_SET_RATE_PARENT;
		clk->hw.init = &init;
		clk->map = regmap;
		clk->reg = desc[i].reg;
		clk->shift = desc[i].shift;
		clk->size = desc[i].size;

		res = devm_clk_hw_register(dev, &clk->hw);
		if (res)
			return dev_err_probe(dev, res, "Failed to register clk %s\n", desc[i].name);
	}

	return 0;
}

static u8 zte_clk_regmap_mux_get_parent(struct clk_hw *hw)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);
	unsigned int val;
	int ret;

	ret = regmap_read(clk->map, clk->reg, &val);
	if (ret)
		return 0xff;

	val >>= clk->shift;
	val &= GENMASK(clk->size - 1, 0);
	return clk_mux_val_to_index(hw, NULL, 0, val);
}

static int zte_clk_regmap_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct zte_clk_regmap *clk = to_zte_clk_regmap(hw);
	unsigned int val = clk_mux_index_to_val(NULL, 0, index);

	return regmap_update_bits(clk->map, clk->reg,
				  GENMASK(clk->size - 1, 0) << clk->shift,
				  val << clk->shift);
}

static int zte_clk_regmap_mux_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, 0);
}

static const struct clk_ops zte_clk_regmap_mux_ops = {
	.get_parent = zte_clk_regmap_mux_get_parent,
	.set_parent = zte_clk_regmap_mux_set_parent,
	.determine_rate = zte_clk_regmap_mux_determine_rate,
};

int zx_clk_register_muxes(struct device *dev, struct regmap *regmap,
			  const struct zx_mux_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks)
{
	struct zte_clk_regmap *clk;
	unsigned int i;
	int res;

	for (i = 0; i < num; ++i) {
		struct clk_init_data init = {};

		clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
		if (!clk)
			return -ENOMEM;

		init.name = desc[i].name;
		init.ops = &zte_clk_regmap_mux_ops;
		init.parent_names = desc[i].parents;
		init.num_parents = desc[i].num_parents;
		clk->hw.init = &init;
		clk->map = regmap;
		clk->reg = desc[i].reg;
		clk->shift = desc[i].shift;
		clk->size = desc[i].size;

		res = devm_clk_hw_register(dev, &clk->hw);
		if (res)
			return dev_err_probe(dev, res, "Failed to register clk %s\n", desc[i].name);

		if (desc[i].id)
			clocks->hws[desc[i].id] = &clk->hw;
	}

	return 0;
}
