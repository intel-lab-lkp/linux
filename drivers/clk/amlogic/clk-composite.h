/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_COMPOSITE_H
#define __AML_CLK_COMPOSITE_H

#include <linux/clk-provider.h>

struct aml_clk_composite_data {
	unsigned int	reg_offset;
	u8		bit_offset;
	u8		div_width;
	u32		*table;
};

extern const struct clk_ops aml_clk_composite_ops;

#endif /* __AML_CLK_COMPOSITE_H */
