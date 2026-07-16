/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#ifndef __DRV_CLK_ZX_H
#define __DRV_CLK_ZX_H

#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define ZX297520V3_PLL_PREPARE_IS_ENABLE	1

/* PLLs, divs and fixed div/factor clocks are never exported directly, only
 * through a gate.
 */

struct zx_pll_desc {
	const char *name;
	const char * const *parents;
	unsigned int num_parents;
	unsigned long rate;
	u16 reg;
	u16 flags;
};

struct zx_fixed_divider_desc {
	const char	*name;
	const char	*parent;
	unsigned int	div;
};

struct zx_mux_desc {
	unsigned int id;
	const char *name;
	const char * const *parents;
	unsigned int num_parents;
	u16 reg;
	u8 shift, size;
};

struct zx_div_desc {
	const char *name, *parent;
	u16 reg;
	u8 shift, size;
};

struct zx_gate_desc {
	unsigned int id;
	const char *name, *parent;
	unsigned long flags;
	u16 reg;
	u8 shift;
};

int zx_clk_register_plls(struct device *dev, struct regmap *regmap,
			 const struct zx_pll_desc *desc, unsigned int num);
int zx_clk_register_muxes(struct device *dev, struct regmap *regmap,
			  const struct zx_mux_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks);
int zx_clk_register_dividers(struct device *dev, struct regmap *regmap,
			     const struct zx_div_desc *desc, unsigned int num);
int zx_clk_register_gates(struct device *dev, struct regmap *regmap,
			  const struct zx_gate_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks);

struct zx_clk_data {
	int (*init)(struct regmap *map);
	const char * const *inputs_enable;
	unsigned int num_inputs_enable;
	const char * const *inputs;
	unsigned int num_inputs;
	const struct zx_pll_desc *plls;
	unsigned int num_plls;
	const struct zx_fixed_divider_desc *fixed_divs;
	unsigned int num_fixed_divs;
	const struct zx_mux_desc *muxes;
	unsigned int num_muxes;
	const struct zx_div_desc *divs;
	unsigned int num_divs;
	const struct zx_gate_desc *gates;
	unsigned int num_gates;
};

int zx_clk_common_probe(struct device *dev, struct device_node *of_node,
			const struct zx_clk_data *data);

#endif /* __DRV_CLK_ZX_H */
