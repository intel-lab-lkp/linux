// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ambarella, Inc.
 *
 * Regmap-backed mux + divider, derived from the vendor composite-clock driver.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/regmap.h>

#include "ccu_mux_div.h"

struct amb_mux {
	struct clk_hw hw;
	struct regmap *map;
	u32 offset;
	u32 mask;
	u32 shift;
};

struct amb_div {
	struct clk_hw hw;
	struct regmap *map;
	u32 offset;
	u32 shift;
	u32 width;
	u32 flags;
	u32 fix_divider;
};

#define to_amb_mux(_hw)	container_of(_hw, struct amb_mux, hw)
#define to_amb_div(_hw)	container_of(_hw, struct amb_div, hw)

static u8 amb_mux_get_parent(struct clk_hw *hw)
{
	struct amb_mux *mux = to_amb_mux(hw);
	u32 val;

	regmap_read(mux->map, mux->offset, &val);
	val >>= mux->shift;
	val &= mux->mask;

	return val;
}

static int amb_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct amb_mux *mux = to_amb_mux(hw);

	return regmap_update_bits(mux->map, mux->offset,
				  mux->mask << mux->shift,
				  index << mux->shift);
}

static const struct clk_ops amb_mux_ops = {
	.get_parent = amb_mux_get_parent,
	.set_parent = amb_mux_set_parent,
};

static unsigned long amb_div_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct amb_div *div = to_amb_div(hw);
	unsigned long rate;
	u32 val;

	regmap_read(div->map, div->offset, &val);

	/* Divider reset/disable bit sits above the field at shift+width. */
	if (val & (BIT(div->width) << div->shift))
		return 0;

	val >>= div->shift;
	val &= clk_div_mask(div->width);

	rate = divider_recalc_rate(hw, parent_rate, val, NULL,
				   div->flags, div->width);
	if (div->fix_divider)
		rate = div_u64(rate, div->fix_divider);

	return rate;
}

static int amb_div_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct amb_div *div = to_amb_div(hw);
	struct clk_rate_request scaled = *req;
	int ret;

	if (!req->rate && (div->flags & CLK_DIVIDER_ONE_BASED))
		return 0;

	if (div->fix_divider) {
		scaled.rate = min(req->rate,
				  ULONG_MAX / div->fix_divider) *
			      div->fix_divider;
		scaled.min_rate = min(req->min_rate,
				      ULONG_MAX / div->fix_divider) *
				  div->fix_divider;
		scaled.max_rate = min(req->max_rate,
				      ULONG_MAX / div->fix_divider) *
				  div->fix_divider;
	}

	ret = divider_determine_rate(hw, &scaled, NULL, div->width,
				     div->flags);
	if (ret)
		return ret;

	req->rate = scaled.rate;
	req->best_parent_rate = scaled.best_parent_rate;
	req->best_parent_hw = scaled.best_parent_hw;
	if (div->fix_divider)
		req->rate = div_u64(req->rate, div->fix_divider);

	return 0;
}

static int amb_div_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct amb_div *div = to_amb_div(hw);
	int val, mask;

	if (div->fix_divider)
		rate *= div->fix_divider;

	if (!rate) {
		/* Assert the reset/disable bit above the divider field. */
		val = BIT(div->width);
		mask = clk_div_mask(div->width + 1);
	} else {
		val = divider_get_val(rate, parent_rate, NULL,
				      div->width, div->flags);
		if (val < 0)
			return val;

		mask = (div->flags & CLK_DIVIDER_ONE_BASED) ?
			clk_div_mask(div->width + 1) : clk_div_mask(div->width);
	}

	regmap_update_bits(div->map, div->offset, mask << div->shift,
			   val << div->shift);

	/*
	 * Non-ONE_BASED dividers use bit 0 as a write-enable strobe. Skip the
	 * pulse when shift == 0 so we do not corrupt the divider field itself.
	 */
	if (!(div->flags & CLK_DIVIDER_ONE_BASED) && div->shift) {
		regmap_update_bits(div->map, div->offset, BIT(0), BIT(0));
		regmap_update_bits(div->map, div->offset, BIT(0), 0);
	}

	return 0;
}

static const struct clk_ops amb_div_ops = {
	.recalc_rate = amb_div_recalc_rate,
	.determine_rate = amb_div_determine_rate,
	.set_rate = amb_div_set_rate,
};

struct clk_hw *amb_div_register(struct device *dev, struct regmap *map,
				const char *name, const struct clk_hw *parent,
				u32 div_reg, u32 div_shift, u32 div_width,
				u32 div_flags, u32 fix_divider)
{
	struct amb_div *div;
	struct clk_init_data init = {};
	int ret;

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	div->map = map;
	div->offset = div_reg;
	div->shift = div_shift;
	div->width = div_width;
	div->flags = div_flags;
	div->fix_divider = fix_divider;

	init.name = name;
	init.ops = &amb_div_ops;
	init.parent_hws = &parent;
	init.num_parents = 1;

	div->hw.init = &init;

	ret = devm_clk_hw_register(dev, &div->hw);
	if (ret)
		return ERR_PTR(ret);

	return &div->hw;
}

struct clk_hw *amb_mux_div_register(struct device *dev, struct regmap *map,
				    const struct amb_mux_div_desc *desc,
				    const struct clk_parent_data *parent_data)
{
	struct amb_mux *mux;
	struct amb_div *div;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!mux || !div)
		return ERR_PTR(-ENOMEM);

	mux->map = map;
	mux->offset = desc->mux_reg;
	mux->shift = desc->mux_shift;
	mux->mask = desc->mux_mask;

	div->map = map;
	div->offset = desc->div_reg;
	div->shift = desc->div_shift;
	div->width = desc->div_width;
	div->flags = desc->div_flags;
	div->fix_divider = desc->fix_divider;

	return devm_clk_hw_register_composite_pdata(dev, desc->name,
						    parent_data,
						    desc->num_parents,
						    &mux->hw, &amb_mux_ops,
						    &div->hw, &amb_div_ops,
						    NULL, NULL,
						    CLK_SET_RATE_NO_REPARENT);
}
