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
	AML_CLKTYPE_DUALDIV	= 6,
	AML_CLKTYPE_PLL		= 7,
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

struct regmap *aml_clk_regmap_init(struct platform_device *pdev);
int of_aml_clk_regs_init(struct device *dev);
u32 of_aml_clk_get_count(struct device_node *np);
const char *of_aml_clk_get_name_index(struct device_node *np, u32 index);
int of_aml_clk_get_parent_num(struct device *dev, int start_index,
			      int end_index);
int of_aml_clk_get_parent_data(struct device *dev, struct clk_hw **dev_hws,
			       int start_index, int end_index,
			       struct clk_parent_data *out_pdatas,
			       u8 *out_num_parents);
u32 *of_aml_clk_get_parent_table(struct device *dev, int start_index,
				 int end_index);
int of_aml_clk_register(struct device *dev, struct clk_hw *hw, int clkid);

#endif /* __AML_CLK_H */
