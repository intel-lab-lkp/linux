// SPDX-License-Identifier: GPL-2.0

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/slab.h>

static inline u32 clk_div_regmap_readl(struct clk_divider_regmap *divider)
{
	u32 val;

	regmap_read(divider->regmap, divider->map_offset, &val);

	return val;
}

static inline void clk_div_regmap_writel(struct clk_divider_regmap *divider, u32 val)
{
	regmap_write(divider->regmap, divider->map_offset, val);

}

static unsigned long clk_divider_regmap_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_divider_regmap *divider = to_clk_divider_regmap(hw);
	unsigned int val;

	val = clk_div_regmap_readl(divider) >> divider->shift;
	val &= clk_div_mask(divider->width);

	return divider_recalc_rate(hw, parent_rate, val, divider->table,
				   divider->flags, divider->width);
}

static long clk_divider_regmap_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *prate)
{
	struct clk_divider_regmap *divider = to_clk_divider_regmap(hw);

	/* if read only, just return current value */
	if (divider->flags & CLK_DIVIDER_READ_ONLY) {
		u32 val;

		val = clk_div_regmap_readl(divider) >> divider->shift;
		val &= clk_div_mask(divider->width);

		return divider_ro_round_rate(hw, rate, prate, divider->table,
					     divider->width, divider->flags,
					     val);
	}

	return divider_round_rate(hw, rate, prate, divider->table,
				  divider->width, divider->flags);
}

static int clk_divider_regmap_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct clk_divider_regmap *divider = to_clk_divider_regmap(hw);

	/* if read only, just return current value */
	if (divider->flags & CLK_DIVIDER_READ_ONLY) {
		u32 val;

		val = clk_div_regmap_readl(divider) >> divider->shift;
		val &= clk_div_mask(divider->width);

		return divider_ro_determine_rate(hw, req, divider->table,
						 divider->width,
						 divider->flags, val);
	}

	return divider_determine_rate(hw, req, divider->table, divider->width,
				      divider->flags);
}

static int clk_divider_regmap_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct clk_divider_regmap *divider = to_clk_divider_regmap(hw);
	int value;
	unsigned long flags = 0;
	u32 val;

	value = divider_get_val(rate, parent_rate, divider->table,
				divider->width, divider->flags);
	if (value < 0)
		return value;

	if (divider->lock)
		spin_lock_irqsave(divider->lock, flags);
	else
		__acquire(divider->lock);

	if (divider->flags & CLK_DIVIDER_HIWORD_MASK) {
		val = clk_div_mask(divider->width) << (divider->shift + 16);
	} else {
		val = clk_div_regmap_readl(divider);
		val &= ~(clk_div_mask(divider->width) << divider->shift);
	}
	val |= (u32)value << divider->shift;
	clk_div_regmap_writel(divider, val);

	if (divider->lock)
		spin_unlock_irqrestore(divider->lock, flags);
	else
		__release(divider->lock);

	return 0;
}

const struct clk_ops clk_divider_regmap_ops = {
	.recalc_rate = clk_divider_regmap_recalc_rate,
	.round_rate = clk_divider_regmap_round_rate,
	.determine_rate = clk_divider_regmap_determine_rate,
	.set_rate = clk_divider_regmap_set_rate,
};
EXPORT_SYMBOL_GPL(clk_divider_regmap_ops);

const struct clk_ops clk_divider_regmap_ro_ops = {
	.recalc_rate = clk_divider_regmap_recalc_rate,
	.round_rate = clk_divider_regmap_round_rate,
	.determine_rate = clk_divider_regmap_determine_rate,
};
EXPORT_SYMBOL_GPL(clk_divider_regmap_ro_ops);

struct clk_hw *__clk_hw_register_divider_regmap(struct device *dev,
		struct device_node *np, const char *name,
		const char *parent_name, const struct clk_hw *parent_hw,
		const struct clk_parent_data *parent_data, unsigned long flags,
		struct regmap *regmap, u8 map_offset, u8 shift, u8 width,
		u8 clk_divider_flags, const struct clk_div_table *table,
		spinlock_t *lock)
{
	struct clk_divider_regmap *div;
	struct clk_hw *hw;
	struct clk_init_data init = {};
	int ret;

	if (clk_divider_flags & CLK_DIVIDER_HIWORD_MASK) {
		if (width + shift > 16) {
			pr_warn("divider value exceeds LOWORD field\n");
			return ERR_PTR(-EINVAL);
		}
	}

	/* allocate the divider */
	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	if (clk_divider_flags & CLK_DIVIDER_READ_ONLY)
		init.ops = &clk_divider_regmap_ro_ops;
	else
		init.ops = &clk_divider_regmap_ops;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.parent_hws = parent_hw ? &parent_hw : NULL;
	init.parent_data = parent_data;
	if (parent_name || parent_hw || parent_data)
		init.num_parents = 1;
	else
		init.num_parents = 0;

	/* struct clk_divider assignments */
	div->regmap = regmap;
	div->map_offset = map_offset;
	div->shift = shift;
	div->width = width;
	div->flags = clk_divider_flags;
	div->lock = lock;
	div->hw.init = &init;
	div->table = table;

	/* register the clock */
	hw = &div->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(div);
		hw = ERR_PTR(ret);
	}

	return hw;
}
EXPORT_SYMBOL_GPL(__clk_hw_register_divider_regmap);

struct clk *clk_register_divider_regmap_table(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags,
		struct regmap *regmap, u8 map_offset, u8 shift, u8 width,
		u8 clk_divider_flags, const struct clk_div_table *table,
		spinlock_t *lock)
{
	struct clk_hw *hw;

	hw =  __clk_hw_register_divider_regmap(dev, NULL, name, parent_name, NULL,
					       NULL, flags, regmap, map_offset,
					       shift, width, clk_divider_flags,
					       table, lock);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}
EXPORT_SYMBOL_GPL(clk_register_divider_regmap_table);

void clk_unregister_divider_regmap(struct clk *clk)
{
	struct clk_divider_regmap *div;
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	div = to_clk_divider_regmap(hw);

	clk_unregister(clk);
	kfree(div);
}
EXPORT_SYMBOL_GPL(clk_unregister_divider_regmap);

/**
 * clk_hw_unregister_divider_regmap - unregister a clk divider
 * @hw: hardware-specific clock data to unregister
 */
void clk_hw_unregister_divider_regmap(struct clk_hw *hw)
{
	struct clk_divider_regmap *div;

	div = to_clk_divider_regmap(hw);

	clk_hw_unregister(hw);
	kfree(div);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister_divider_regmap);

static void devm_clk_hw_release_divider_regmap(struct device *dev, void *res)
{
	clk_hw_unregister_divider_regmap(*(struct clk_hw **)res);
}

struct clk_hw *__devm_clk_hw_register_divider_regmap(struct device *dev,
		struct device_node *np, const char *name,
		const char *parent_name, const struct clk_hw *parent_hw,
		const struct clk_parent_data *parent_data, unsigned long flags,
		struct regmap *regmap, u8 map_offset, u8 shift, u8 width,
		u8 clk_divider_flags, const struct clk_div_table *table,
		spinlock_t *lock)
{
	struct clk_hw **ptr, *hw;

	ptr = devres_alloc(devm_clk_hw_release_divider_regmap, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	hw = __clk_hw_register_divider_regmap(dev, np, name, parent_name, parent_hw,
					      parent_data, flags, regmap, map_offset,
					      shift, width, clk_divider_flags, table,
					      lock);

	if (!IS_ERR(hw)) {
		*ptr = hw;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return hw;
}
EXPORT_SYMBOL_GPL(__devm_clk_hw_register_divider_regmap);
