// SPDX-License-Identifier: GPL-2.0

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

/**
 * DOC: basic gatable clock which can gate and ungate its output
 *
 * Traits of this clock:
 * prepare - clk_(un)prepare only ensures parent is (un)prepared
 * enable - clk_enable and clk_disable are functional & control gating
 * rate - inherits rate from parent.  No clk_set_rate support
 * parent - fixed parent.  No clk_set_parent support
 */

static inline u32 clk_gate_regmap_readl(struct clk_gate_regmap *gate)
{
	u32 val;

	regmap_read(gate->map, gate->map_offset, &val);

	return val;
}

static inline void clk_gate_regmap_writel(struct clk_gate_regmap *gate, u32 val)
{
	regmap_write(gate->map, gate->map_offset, val);

}

/*
 * It works on following logic:
 *
 * For enabling clock, enable = 1
 *	set2dis = 1	-> clear bit	-> set = 0
 *	set2dis = 0	-> set bit	-> set = 1
 *
 * For disabling clock, enable = 0
 *	set2dis = 1	-> set bit	-> set = 1
 *	set2dis = 0	-> clear bit	-> set = 0
 *
 * So, result is always: enable xor set2dis.
 */
static void clk_gate_regmap_endisable(struct clk_hw *hw, int enable)
{
	struct clk_gate_regmap *gate = to_clk_gate_regmap(hw);
	int set = gate->flags & CLK_GATE_SET_TO_DISABLE ? 1 : 0;
	unsigned long flags;
	u32 reg;

	set ^= enable;

	if (gate->lock)
		spin_lock_irqsave(gate->lock, flags);
	else
		__acquire(gate->lock);

	if (gate->flags & CLK_GATE_HIWORD_MASK) {
		reg = BIT(gate->bit_idx + 16);
		if (set)
			reg |= BIT(gate->bit_idx);
	} else {
		reg = clk_gate_regmap_readl(gate);

		if (set)
			reg |= BIT(gate->bit_idx);
		else
			reg &= ~BIT(gate->bit_idx);
	}

	clk_gate_regmap_writel(gate, reg);

	if (gate->lock)
		spin_unlock_irqrestore(gate->lock, flags);
	else
		__release(gate->lock);
}

static int clk_gate_regmap_enable(struct clk_hw *hw)
{
	clk_gate_regmap_endisable(hw, 1);

	return 0;
}

static void clk_gate_regmap_disable(struct clk_hw *hw)
{
	clk_gate_regmap_endisable(hw, 0);
}

int clk_gate_regmap_is_enabled(struct clk_hw *hw)
{
	u32 reg;
	struct clk_gate_regmap *gate = to_clk_gate_regmap(hw);

	reg = clk_gate_regmap_readl(gate);

	/* if a set bit disables this clk, flip it before masking */
	if (gate->flags & CLK_GATE_SET_TO_DISABLE)
		reg ^= BIT(gate->bit_idx);

	reg &= BIT(gate->bit_idx);

	return reg ? 1 : 0;
}
EXPORT_SYMBOL_GPL(clk_gate_regmap_is_enabled);

const struct clk_ops clk_gate_regmap_ops = {
	.enable = clk_gate_regmap_enable,
	.disable = clk_gate_regmap_disable,
	.is_enabled = clk_gate_regmap_is_enabled,
};
EXPORT_SYMBOL_GPL(clk_gate_regmap_ops);

struct clk_hw *__clk_hw_register_gate_regmap(struct device *dev,
		struct device_node *np, const char *name,
		const char *parent_name, const struct clk_hw *parent_hw,
		const struct clk_parent_data *parent_data,
		unsigned long flags,
		struct regmap *map, u8 map_offset, u8 bit_idx,
		u8 clk_gate_flags, spinlock_t *lock)
{
	struct clk_gate_regmap *gate;
	struct clk_hw *hw;
	struct clk_init_data init = {};
	int ret = -EINVAL;

	if (clk_gate_flags & CLK_GATE_HIWORD_MASK) {
		if (bit_idx > 15) {
			pr_err("gate bit exceeds LOWORD field\n");
			return ERR_PTR(-EINVAL);
		}
	}

	/* allocate the gate */
	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_gate_regmap_ops;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.parent_hws = parent_hw ? &parent_hw : NULL;
	init.parent_data = parent_data;
	if (parent_name || parent_hw || parent_data)
		init.num_parents = 1;
	else
		init.num_parents = 0;

	/* struct clk_gate_regmap assignments */
	gate->map = map;
	gate->map_offset = map_offset;
	gate->bit_idx = bit_idx;
	gate->flags = clk_gate_flags;
	gate->lock = lock;
	gate->hw.init = &init;

	hw = &gate->hw;
	if (dev || !np)
		ret = clk_hw_register(dev, hw);
	else if (np)
		ret = of_clk_hw_register(np, hw);
	if (ret) {
		kfree(gate);
		hw = ERR_PTR(ret);
	}

	return hw;

}
EXPORT_SYMBOL_GPL(__clk_hw_register_gate_regmap);

struct clk *clk_register_gate_regmap(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags, struct regmap *map,
		u8 map_offset, u8 bit_idx, u8 clk_gate_flags, spinlock_t *lock)
{
	struct clk_hw *hw;

	hw = __clk_hw_register_gate_regmap(dev, NULL, name, parent_name, NULL,
					   NULL, flags, map, map_offset, bit_idx,
					   clk_gate_flags, lock);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}
EXPORT_SYMBOL_GPL(clk_register_gate_regmap);

void clk_unregister_gate_regmap(struct clk *clk)
{
	struct clk_gate_regmap *gate;
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	gate = to_clk_gate_regmap(hw);

	clk_unregister(clk);
	kfree(gate);
}
EXPORT_SYMBOL_GPL(clk_unregister_gate_regmap);

void clk_hw_unregister_gate_regmap(struct clk_hw *hw)
{
	struct clk_gate_regmap *gate;

	gate = to_clk_gate_regmap(hw);

	clk_hw_unregister(hw);
	kfree(gate);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister_gate_regmap);

static void devm_clk_hw_release_gate_regmap(struct device *dev, void *res)
{
	clk_hw_unregister_gate_regmap(*(struct clk_hw **)res);
}

struct clk_hw *__devm_clk_hw_register_gate_regmap(struct device *dev,
		struct device_node *np, const char *name,
		const char *parent_name, const struct clk_hw *parent_hw,
		const struct clk_parent_data *parent_data,
		unsigned long flags, struct regmap *map,
		u8 map_offset, u8 bit_idx,
		u8 clk_gate_flags, spinlock_t *lock)
{
	struct clk_hw **ptr, *hw;

	ptr = devres_alloc(devm_clk_hw_release_gate_regmap, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	hw = __clk_hw_register_gate_regmap(dev, np, name, parent_name, parent_hw,
					   parent_data, flags, map, map_offset,
					   bit_idx, clk_gate_flags, lock);

	if (!IS_ERR(hw)) {
		*ptr = hw;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return hw;
}
EXPORT_SYMBOL_GPL(__devm_clk_hw_register_gate_regmap);
