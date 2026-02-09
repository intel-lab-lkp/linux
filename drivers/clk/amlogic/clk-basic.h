/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_BASIC_H
#define __AML_CLK_BASIC_H

#include <linux/clk-provider.h>

struct aml_clk_mux_data {
	unsigned int	reg_offset;
	u32		*table;
	u32		mask;
	u8		shift;
	u8		flags;
};

struct aml_clk_divider_data {
	unsigned int	reg_offset;
	u8		shift;
	u8		width;
	u16		flags;
	struct clk_div_table	*table;
};

struct aml_clk_gate_data {
	unsigned int	reg_offset;
	u8		bit_idx;
	u8		flags;
};

extern const struct clk_ops aml_clk_gate_ops;
extern const struct clk_ops aml_clk_divider_ops;
extern const struct clk_ops aml_clk_divider_ro_ops;
extern const struct clk_ops aml_clk_mux_ops;
extern const struct clk_ops aml_clk_mux_ro_ops;

#endif /* __AML_CLK_BASIC_H */
