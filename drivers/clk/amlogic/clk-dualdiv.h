/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AML_CLK_DUALDIV_H
#define __AML_CLK_DUALDIV_H

#include <linux/clk-provider.h>

struct aml_clk_dualdiv_param {
	unsigned int div_mode;
	unsigned int n0;
	unsigned int m0;
	unsigned int n1;
	unsigned int m1;
};

struct aml_clk_dualdiv_data {
	unsigned int			reg_offset;
	struct	aml_clk_dualdiv_param	*table;
	unsigned int			table_count;
};

extern const struct clk_ops aml_clk_dualdiv_ops;

#endif /* __AML_CLK_DUALDIV_MUX_H */
