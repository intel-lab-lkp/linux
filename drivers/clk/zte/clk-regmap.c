// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Copyright (c) 2018 BayLibre, SAS.
 * Copyright (c) 2026 Stefan Dösinger.
 * Author: Stefan Dösinger <stefandoesinger@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/errno.h>

#include "clk-zx.h"

int zx_clk_register_gates(struct device *dev, struct regmap *regmap,
			  const struct zx_gate_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks)
{
	return -ENODEV;
}

int zx_clk_register_dividers(struct device *dev, struct regmap *regmap,
			     const struct zx_div_desc *desc, unsigned int num)
{
	return -ENODEV;
}

int zx_clk_register_muxes(struct device *dev, struct regmap *regmap,
			  const struct zx_mux_desc *desc, unsigned int num,
			  struct clk_hw_onecell_data *clocks)
{
	return -ENODEV;
}
