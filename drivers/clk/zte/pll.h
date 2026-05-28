/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#ifndef __DRV_CLK_ZTE_PLL_H
#define __DRV_CLK_ZTE_PLL_H

#include <linux/clk-provider.h>
#include <linux/types.h>

struct zx29_pll_desc {
	u32 reg;
	const char *name;
	const char * const *parents;
	unsigned int num_parents;
	unsigned long rate;
};

int zx29_register_plls(struct device *dev, void __iomem *base, const struct zx29_pll_desc *plls,
		       unsigned int count);

#endif /* __DRV_CLK_ZTE_PLL_H */
