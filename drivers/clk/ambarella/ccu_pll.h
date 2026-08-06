/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ambarella, Inc.
 */

#ifndef __CCU_PLL_H
#define __CCU_PLL_H

#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/types.h>

enum {
	CTRL_OFFSET = 0,
	FRAC_OFFSET,
	CTRL2_OFFSET,
	CTRL3_OFFSET,
	PRES_OFFSET,
	POST_OFFSET,
	REG_NUM,
};

#define CTRL_BYPASS		BIT(2)
#define CTRL_WRITE_ENABLE	BIT(0)
#define CTRL_FRAC_MODE		BIT(3)
#define CTRL_FORCE_RESET	BIT(4)
#define CTRL_POWER_DOWN		BIT(5)
#define CTRL_HALT_VCO		BIT(6)
#define CTRL_VCODIV_DIV2	BIT(8)
#define CTRL_FSDIV_DIV2		BIT(9)
#define CTRL_FSOUT_DIV2		BIT(10)
#define CTRL_BYPASS_HSDIV	BIT(11)

#define CTRL2_VCODIV_DIV2	BIT(8)
#define CTRL2_FSDIV_DIV2	BIT(9)
#define CTRL2_FSOUT_DIV2	BIT(11)
#define CTRL2_BYPASS_HSDIV	BIT(12)

#define CTRL3_VCO_RANGE_MASK	0x6
#define CTRL3_VCO_CLAMP		0x8

struct amb_pll_soc_data {
	u32 pll_version;
	u32 fsout_mask;
	u32 fsout_val;
	u32 fsdiv_mask;
	u32 fsdiv_val;
	u32 vcodiv_mask;
	u32 vcodiv_val;
	u32 vco_max_mhz;
	u32 vco_min_mhz;
	u32 vco_range[4];
	u32 ctrl2_val;
	u32 ctrl3_val;
};

struct amb_pll_desc {
	const char *name;
	const struct clk_hw *parent;
	const u32 reg_offset[REG_NUM];
	const struct amb_pll_soc_data *soc_data;
	bool frac_mode;
};

struct clk_hw *amb_pll_register(struct device *dev, struct regmap *map,
				const struct amb_pll_desc *desc);

#endif
