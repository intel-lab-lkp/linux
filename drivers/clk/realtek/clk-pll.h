/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_CLK_PLL_H
#define __CLK_REALTEK_CLK_PLL_H

#include <linux/spinlock.h>
#include "clk-rtk-common.h"
#include "freq_table.h"

struct reg_sequence;

struct rtk_clk_regmap_pll {
	struct rtk_clk_regmap clkr;
	const struct reg_sequence *seq_power_on;
	u32 num_seq_power_on;
	const struct reg_sequence *seq_power_off;
	u32 num_seq_power_off;
	const struct reg_sequence *seq_pre_set_freq;
	u32 num_seq_pre_set_freq;
	const struct reg_sequence *seq_post_set_freq;
	u32 num_seq_post_set_freq;
	const struct rtk_freq_table *freq_tbl;
	u32 freq_reg;
	u32 freq_mask;
	u32 freq_ready_mask;
	u32 freq_ready_reg;
	u32 freq_ready_val;
	u32 power_reg;
	u32 power_mask;
	u32 power_val_on;

	/* This lock prevents race conditions when multiple CPUs or contexts
	 * simultaneously access this PLL's registers during multi-step operations
	 */
	spinlock_t lock;
};

#define __rtk_clk_regmap_pll_hw(_ptr)  __rtk_clk_regmap_hw(&(_ptr)->clkr)

extern const struct clk_ops rtk_clk_pll_ops;
extern const struct clk_ops rtk_clk_pll_ro_ops;

struct rtk_clk_regmap_pll_mmc {
	struct rtk_clk_regmap clkr;
	unsigned int pll_ofs;
	unsigned int ssc_dig_ofs;
	struct clk_hw phase0_hw;
	struct clk_hw phase1_hw;
};

#define __rtk_clk_regmap_pll_mmc_hw(_ptr)  __rtk_clk_regmap_hw(&(_ptr)->clkr)

extern const struct clk_ops rtk_clk_pll_mmc_ops;
extern const struct clk_ops rtk_clk_pll_mmc_phase_ops;

#endif /* __CLK_REALTEK_CLK_PLL_H */
