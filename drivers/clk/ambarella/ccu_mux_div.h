/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ambarella, Inc.
 */

#ifndef __CCU_MUX_DIV_H
#define __CCU_MUX_DIV_H

#include <linux/clk-provider.h>
#include <linux/regmap.h>

struct amb_mux_div_desc {
	const char *name;
	const int *parents;
	u8 num_parents;
	u32 mux_reg;
	u32 mux_shift;
	u32 mux_mask;
	u32 div_reg;
	u32 div_shift;
	u32 div_width;
	u32 div_flags;
	u32 fix_divider;
};

struct clk_hw *amb_mux_div_register(struct device *dev, struct regmap *map,
				    const struct amb_mux_div_desc *desc,
				    const struct clk_parent_data *parent_data);

struct clk_hw *amb_div_register(struct device *dev, struct regmap *map,
				const char *name, const struct clk_hw *parent,
				u32 div_reg, u32 div_shift, u32 div_width,
				u32 div_flags, u32 fix_divider);

#endif
