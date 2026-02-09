// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/module.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/err.h>

#include "clk.h"
#include "clk-basic.h"
#include "clk-composite.h"

static const struct {
	unsigned int type;
	const char *name;
} clk_types[] = {
#define ENTRY(f) { f, #f }
	ENTRY(AML_CLKTYPE_MUX),
	ENTRY(AML_CLKTYPE_DIV),
	ENTRY(AML_CLKTYPE_GATE),
	ENTRY(AML_CLKTYPE_COMPOSITE),
#undef ENTRY
};

static int aml_clk_type_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct aml_clk *clk = to_aml_clk(hw);
	int i;

	for (i = 0; i < ARRAY_SIZE(clk_types); i++) {
		if (clk_types[i].type == clk->type) {
			seq_printf(s, "%s\n", clk_types[i].name);
			return 0;
		}
	}

	seq_puts(s, "UNKNOWN\n");

	return -EINVAL;
}

static int aml_clk_type_open(struct inode *inode, struct file *file)
{
	return single_open(file, aml_clk_type_show, inode->i_private);
}

const struct file_operations aml_clk_type_fops = {
	.owner		= THIS_MODULE,
	.open		= aml_clk_type_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
EXPORT_SYMBOL_NS_GPL(aml_clk_type_fops, "CLK_AMLOGIC");

/*
 * SoC HW design constrains the maximum frequency for each clock network.
 * Configuring frequencies beyond these limits may cause module malfunction
 * or even crosstalk affecting other modules.
 *
 * This function synthesizes the HW-constrained frequency range and the
 * divider's capability to output the permissible frequency range for the
 * current clock.
 */
static int aml_clk_div_available_rates_show(struct seq_file *s, void *data)
{
	struct clk_hw *hw = s->private;
	struct clk_hw *phw = clk_hw_get_parent(hw);
	struct aml_clk *clk = to_aml_clk(hw);
	unsigned long min, max, prate;
	unsigned long range_min, range_max;
	unsigned int div_val;
	unsigned long div_width, div_flags = 0;
	const struct clk_div_table *div_table = NULL;

	if (!phw) {
		pr_err("%s: Can't get parent\n", clk_hw_get_name(hw));

		return -ENOENT;
	}

	prate = clk_hw_get_rate(phw);
	clk_hw_get_rate_range(hw, &range_min, &range_max);
	max = prate;
	if (clk->type == AML_CLKTYPE_DIV) {
		struct aml_clk_divider_data *div = clk->data;

		if (div->flags & CLK_DIVIDER_READ_ONLY) {
			min = prate;
			goto out_printf;
		} else {
			div_val = (1 << div->width) - 1;
			div_table = div->table;
			div_flags = div->flags;
			div_width = div->width;
		}
	} else if (clk->type == AML_CLKTYPE_COMPOSITE) {
		struct aml_clk_composite_data *composite = clk->data;

		div_val = (1 << composite->div_width) - 1;
		div_width = composite->div_width;
	} else {
		pr_err("%s: Unsupported clock type\n", clk_hw_get_name(hw));
		return -EINVAL;
	}

	min = divider_recalc_rate(hw, prate, div_val, div_table, div_flags,
				  div_width);

	clk_hw_get_rate_range(hw, &range_min, &range_max);
	if (range_min > min)
		min = range_min;

	if (range_max < max)
		max = range_max;

	min = divider_round_rate(hw, min, &prate, NULL, div_width, 0);
	max = divider_round_rate(hw, max, &prate, NULL, div_width, 0);

out_printf:
	seq_printf(s, "min_rate:%ld\n", min);
	seq_printf(s, "max_rate:%ld\n", max);

	return 0;
}

static int aml_clk_div_available_rates_open(struct inode *inode, struct file *file)
{
	return single_open(file, aml_clk_div_available_rates_show,
			   inode->i_private);
}

const struct file_operations aml_clk_div_available_rates_fops = {
	.owner		= THIS_MODULE,
	.open		= aml_clk_div_available_rates_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
EXPORT_SYMBOL_NS_GPL(aml_clk_div_available_rates_fops, "CLK_AMLOGIC");
#endif /* CONFIG_DEBUG_FS */

MODULE_DESCRIPTION("Amlogic Common Clock Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
