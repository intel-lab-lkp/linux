/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_NOGLITCH_H
#define __AML_CLK_NOGLITCH_H

#include <linux/clk-provider.h>

/*
 * NOTE: Currently, the divider of the AM no-glitch clock uses a fixed 7-bit
 * register field width.
 *
 * To better consolidate the code, the implementation for calculating
 * "clk_available_rates" for divider, composite, and no-glitch clocks has been
 * unified into a single function (in clk.c). Therefore, this macro is defined
 * here for use by the related functions.
 */
#define CLK_NOGLITCH_DIV_WIDTH		7

struct aml_clk_noglitch_data {
	unsigned int	reg_offset;
	u32		*table;
};

extern const struct clk_ops aml_clk_noglitch_ops;

#endif /* __AML_CLK_NOGLITCH_H */
