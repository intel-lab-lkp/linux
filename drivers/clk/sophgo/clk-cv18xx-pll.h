// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Inochi Amaoto <inochiama@outlook.com>
 */

#ifndef _CLK_SOPHGO_CV1800_PLL_H_
#define _CLK_SOPHGO_CV1800_PLL_H_

#include <linux/clk-provider.h>
#include "clk-cv18xx-common.h"

struct cv1800_clk_pll_limit {
	struct {
		u8 min;
		u8 max;
	} pre_div, div, post_div, ictrl, mode;
};

#define _CV1800_PLL_LIMIT(_min, _max)	\
	{				\
		.min = _min,		\
		.max = _max,		\
	}				\

struct cv1800_clk_pll_synthesizer {
	struct cv1800_clk_regbit	en;
	struct cv1800_clk_regbit	clk_half;
	u32				ctrl;
	u32				set;
};

struct cv1800_clk_pll {
	struct cv1800_clk_common		common;
	u32					pll_reg;
	struct cv1800_clk_regbit		pll_pwd;
	struct cv1800_clk_regbit		pll_status;
	const struct cv1800_clk_pll_limit	*pll_limit;
	struct cv1800_clk_pll_synthesizer	*pll_syn;
};

#define CV1800_INTEGRAL_PLL(_name, _parent, _pll_reg,			\
			     _pll_pwd_reg, _pll_pwd_shift,		\
			     _pll_status_reg, _pll_status_shift,	\
			     _pll_limit, _flags)			\
	struct cv1800_clk_pll _name = {					\
		.common		= CV1800_CLK_COMMON(#_name, _parent,	\
						    &cv1800_clk_ipll_ops,\
						    _flags),		\
		.pll_reg	= _pll_reg,				\
		.pll_pwd	= CV1800_CLK_BIT(_pll_pwd_reg,		\
					       _pll_pwd_shift),		\
		.pll_status	= CV1800_CLK_BIT(_pll_status_reg,	\
					       _pll_status_shift),	\
		.pll_limit	= _pll_limit,				\
		.pll_syn	= NULL,					\
	}

#define CV1800_FACTIONAL_PLL(_name, _parent, _pll_reg,			\
			     _pll_pwd_reg, _pll_pwd_shift,		\
			     _pll_status_reg, _pll_status_shift,	\
			     _pll_limit, _pll_syn, _flags)		\
	struct cv1800_clk_pll _name = {					\
		.common		= CV1800_CLK_COMMON(#_name, _parent,	\
						    &cv1800_clk_fpll_ops,\
						    _flags),		\
		.pll_reg	= _pll_reg,				\
		.pll_pwd	= CV1800_CLK_BIT(_pll_pwd_reg,		\
					       _pll_pwd_shift),		\
		.pll_status	= CV1800_CLK_BIT(_pll_status_reg,	\
					       _pll_status_shift),	\
		.pll_limit	= _pll_limit,				\
		.pll_syn	= _pll_syn,				\
	}


extern const struct clk_ops cv1800_clk_ipll_ops;
extern const struct clk_ops cv1800_clk_fpll_ops;

#endif // _CLK_SOPHGO_CV1800_PLL_H_
