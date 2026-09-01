/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_CLK_REGMAP_GATE_H
#define __CLK_REALTEK_CLK_REGMAP_GATE_H

#include "clk-rtk-common.h"

struct rtk_clk_regmap_gate {
	struct rtk_clk_regmap clkr;
	unsigned int gate_ofs;
	u8 bit_idx;
	u32 write_en : 1;
};

#define __rtk_clk_regmap_gate_hw(_p) __rtk_clk_regmap_hw(&(_p)->clkr)

#define __RTK_CLK_REGMAP_GATE(_name, _parent, _ops, _flags, _ofs, _bit_idx, \
			      _write_en)                                    \
	struct rtk_clk_regmap_gate _name = {                                \
		.clkr.hw.init = CLK_HW_INIT(#_name, _parent, _ops, _flags), \
		.gate_ofs = _ofs,                                           \
		.bit_idx = _bit_idx,                                        \
		.write_en = _write_en,                                      \
	}

#define RTK_CLK_REGMAP_GATE(_name, _parent, _flags, _ofs, _bit_idx, _write_en)        \
	__RTK_CLK_REGMAP_GATE(_name, _parent, &rtk_clk_regmap_gate_ops, _flags, _ofs, \
			      _bit_idx, _write_en)

#define RTK_CLK_REGMAP_GATE_RO(_name, _parent, _flags, _ofs, _bit_idx, _write_en)  \
	__RTK_CLK_REGMAP_GATE(_name, _parent, &rtk_clk_regmap_gate_ro_ops, _flags, \
			  _ofs, _bit_idx, _write_en)

#define __RTK_CLK_REGMAP_GATE_NO_PARENT(_name, _ops, _flags, _ofs, _bit_idx,  \
				    _write_en)                                \
	struct rtk_clk_regmap_gate _name = {                                  \
		.clkr.hw.init = CLK_HW_INIT_NO_PARENT(#_name, _ops, _flags),  \
		.gate_ofs = _ofs,                                             \
		.bit_idx = _bit_idx,                                          \
		.write_en = _write_en,                                        \
	}

#define RTK_CLK_REGMAP_GATE_NO_PARENT(_name, _flags, _ofs, _bit_idx, _write_en)        \
	__RTK_CLK_REGMAP_GATE_NO_PARENT(_name, &rtk_clk_regmap_gate_ops, _flags, _ofs, \
				    _bit_idx, _write_en)

#define RTK_CLK_REGMAP_GATE_NO_PARENT_RO(_name, _flags, _ofs, _bit_idx, _write_en)  \
	__RTK_CLK_REGMAP_GATE_NO_PARENT(_name, &rtk_clk_regmap_gate_ro_ops, _flags, \
				    _ofs, _bit_idx, _write_en)

static inline struct rtk_clk_regmap_gate *to_rtk_clk_regmap_gate(struct clk_hw *hw)
{
	struct rtk_clk_regmap *clkr = to_rtk_clk_regmap(hw);

	return container_of(clkr, struct rtk_clk_regmap_gate, clkr);
}

extern const struct clk_ops rtk_clk_regmap_gate_ops;
extern const struct clk_ops rtk_clk_regmap_gate_ro_ops;

#endif /* __CLK_REALTEK_CLK_REGMAP_GATE_H */
