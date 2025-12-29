/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_CLK_PLL_H
#define __CLK_REALTEK_CLK_PLL_H

#include "common.h"
#include "freq_table.h"

struct clk_pll {
	struct clk_regmap clkr;
	const struct reg_sequence *seq_power_on;
	u32 num_seq_power_on;
	const struct reg_sequence *seq_power_off;
	u32 num_seq_power_off;
	const struct reg_sequence *seq_pre_set_freq;
	u32 num_seq_pre_set_freq;
	const struct reg_sequence *seq_post_set_freq;
	u32 num_seq_post_set_freq;
	const struct freq_table *freq_tbl;
	u32 freq_reg;
	u32 freq_mask;
	u32 freq_ready_valid;
	u32 freq_ready_mask;
	u32 freq_ready_reg;
	u32 freq_ready_val;
	u32 power_reg;
	u32 power_mask;
	u32 power_val_on;
};

#define __clk_pll_hw(_ptr)  __clk_regmap_hw(&(_ptr)->clkr)

static inline struct clk_pll *to_clk_pll(struct clk_hw *hw)
{
	struct clk_regmap *clkr = to_clk_regmap(hw);

	return container_of(clkr, struct clk_pll, clkr);
}

extern const struct clk_ops clk_pll_ops;
extern const struct clk_ops clk_pll_ro_ops;

struct clk_pll_mmc {
	struct clk_regmap clkr;
	int pll_ofs;
	int ssc_dig_ofs;
	struct clk_hw phase0_hw;
	struct clk_hw phase1_hw;
	u32 set_rate_val_53_97_set_ipc: 1;
};

#define __clk_pll_mmc_hw(_ptr)  __clk_regmap_hw(&(_ptr)->clkr)

static inline struct clk_pll_mmc *to_clk_pll_mmc(struct clk_hw *hw)
{
	struct clk_regmap *clkr = to_clk_regmap(hw);

	return container_of(clkr, struct clk_pll_mmc, clkr);
}

extern const struct clk_ops clk_pll_mmc_ops;
extern const struct clk_ops clk_pll_mmc_phase_ops;

#endif /* __CLK_REALTEK_CLK_PLL_H */
