// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/errno.h>

#include "clk-zx.h"

int zx_clk_register_plls(struct device *dev, struct regmap *regmap,
			 const struct zx_pll_desc *desc, unsigned int num)
{
	return -ENODEV;
}
