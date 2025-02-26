/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Inochi Amaoto <inochiama@outlook.com>
 */

#ifndef _CLK_SOPHGO_SG2044_H_
#define _CLK_SOPHGO_SG2044_H_

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#define PLL_LIMIT_FOUTVCO	0
#define PLL_LIMIT_FOUT		1
#define PLL_LIMIT_REFDIV	2
#define PLL_LIMIT_FBDIV		3
#define PLL_LIMIT_POSTDIV1	4
#define PLL_LIMIT_POSTDIV2	5

#define for_each_pll_limit_range(_var, _limit) \
	for (_var = (_limit)->min; _var <= (_limit)->max; _var++)

struct sg2044_clk_limit {
	u64 min;
	u64 max;
};

struct sg2044_clk_common {
	struct clk_hw	hw;
	void __iomem	*base;
	spinlock_t	*lock;
	unsigned int	id;
};

struct sg2044_clk_desc_data {
	struct sg2044_clk_common	**pll;
	struct sg2044_clk_common	**div;
	struct sg2044_clk_common	**mux;
	struct sg2044_clk_common	**gate;
	u16				num_pll;
	u16				num_div;
	u16				num_mux;
	u16				num_gate;
};

#define SG2044_CLK_COMMON(_id, _name, _parents, _op, _flags)		\
	{								\
		.hw.init = CLK_HW_INIT_PARENTS_DATA(_name, _parents,	\
						    _op, (_flags)),	\
		.id = (_id),						\
	}

static inline bool sg2044_clk_fit_limit(u64 value,
					const struct sg2044_clk_limit *limit)
{
	return value >= limit->min && value <= limit->max;
}

#define hw_to_sg2044_clk_common(_hw)					\
	container_of((_hw), struct sg2044_clk_common, hw)

#endif /* _CLK_SOPHGO_SG2044_H_ */
