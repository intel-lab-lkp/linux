/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_H
#define __AML_CLK_H

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

enum aml_clk_type {
	AML_CLKTYPE_MUX		= 1,
	AML_CLKTYPE_DIV		= 2,
	AML_CLKTYPE_GATE	= 3,
	AML_CLKTYPE_COMPOSITE	= 4,
	AML_CLKTYPE_NOGLITCH	= 5,
};

struct aml_clk {
	struct clk_hw	hw;
	enum aml_clk_type type;
	struct regmap	*map;
	void		*data;
};

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

extern const struct file_operations aml_clk_type_fops;
extern const struct file_operations aml_clk_div_available_rates_fops;
#endif /* CONFIG_DEBUG_FS */

static inline struct aml_clk *to_aml_clk(struct clk_hw *hw)
{
	return container_of(hw, struct aml_clk, hw);
}

#endif /* __AML_CLK_H */
