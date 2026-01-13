/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_CLK_REGMAP_GATE_H
#define __CLK_REALTEK_CLK_REGMAP_GATE_H

#include "common.h"

struct clk_regmap_gate {
	struct clk_regmap clkr;
	int gate_ofs;
	u8 bit_idx;
	u32 write_en : 1;
};

#define __clk_regmap_gate_hw(_p) __clk_regmap_hw(&(_p)->clkr)

#define __CLK_REGMAP_GATE(_name, _parent, _ops, _flags, _ofs, _bit_idx,     \
			  _write_en)                                        \
	struct clk_regmap_gate _name = {                                    \
		.clkr.hw.init = CLK_HW_INIT(#_name, _parent, _ops, _flags), \
		.gate_ofs = _ofs,                                           \
		.bit_idx = _bit_idx,                                        \
		.write_en = _write_en,                                      \
	}

#define CLK_REGMAP_GATE(_name, _parent, _flags, _ofs, _bit_idx, _write_en)    \
	__CLK_REGMAP_GATE(_name, _parent, &clk_regmap_gate_ops, _flags, _ofs, \
			  _bit_idx, _write_en)

#define CLK_REGMAP_GATE_RO(_name, _parent, _flags, _ofs, _bit_idx, _write_en) \
	__CLK_REGMAP_GATE(_name, _parent, &clk_regmap_gate_ro_ops, _flags,    \
			  _ofs, _bit_idx, _write_en)

#define __CLK_REGMAP_GATE_NO_PARENT(_name, _ops, _flags, _ofs, _bit_idx,     \
				    _write_en)                               \
	struct clk_regmap_gate _name = {                                     \
		.clkr.hw.init = CLK_HW_INIT_NO_PARENT(#_name, _ops, _flags), \
		.gate_ofs = _ofs,                                            \
		.bit_idx = _bit_idx,                                         \
		.write_en = _write_en,                                       \
	}

#define CLK_REGMAP_GATE_NO_PARENT(_name, _flags, _ofs, _bit_idx, _write_en)    \
	__CLK_REGMAP_GATE_NO_PARENT(_name, &clk_regmap_gate_ops, _flags, _ofs, \
				    _bit_idx, _write_en)

#define CLK_REGMAP_GATE_NO_PARENT_RO(_name, _flags, _ofs, _bit_idx, _write_en) \
	__CLK_REGMAP_GATE_NO_PARENT(_name, &clk_regmap_gate_ro_ops, _flags,    \
				    _ofs, _bit_idx, _write_en)

static inline struct clk_regmap_gate *to_clk_regmap_gate(struct clk_hw *hw)
{
	struct clk_regmap *clkr = to_clk_regmap(hw);

	return container_of(clkr, struct clk_regmap_gate, clkr);
}

extern const struct clk_ops clk_regmap_gate_ops;
extern const struct clk_ops clk_regmap_gate_ro_ops;

#endif /* __CLK_REALTEK_CLK_REGMAP_GATE_H */
