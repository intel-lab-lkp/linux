/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_PLL_H
#define __AML_CLK_PLL_H

#include <linux/clk-provider.h>
#include <linux/regmap.h>

struct aml_pll_parms_table {
	unsigned int	m;
	unsigned int	n;
	unsigned int	frac;
	unsigned int	od;
};

struct aml_pll_dco_range {
	unsigned long long	min;
	unsigned long long	max;
};

#define AML_PLL_ROUND_CLOSEST	BIT(0)/* Supports fractional multiplication */
#define AML_PLL_READ_ONLY	BIT(1)
#define AML_PLL_M_EN0P5		BIT(2)/* Multiplication factor is m = m / 2 */

struct aml_pll_data {
	struct aml_pll_parms_table	*table;
	unsigned int			table_count;
	struct aml_pll_dco_range	range;
	unsigned int			frac_max;
	u8				od_max;
	u16				flags;
	/* Save the context information of the PLL */
	int				context_is_enabled;
	unsigned long			context_rate;
};

extern const struct clk_ops aml_pll_ops;
extern const struct clk_ops aml_pll_ro_ops;

#endif /* __AML_CLK_PLL_H */
