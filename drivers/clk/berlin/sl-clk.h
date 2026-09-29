/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SL_CLK_H
#define __SL_CLK_H

#include <linux/compiler_types.h>
#include <linux/spinlock_types.h>

struct clk_hw;
struct device;

struct sl_clk_desc {
	const char	*name;
	u32		id;
	u32		offset;
	unsigned long	flags;
};

struct sl_gateclk_desc {
	const char	*name;
	const char	*parent_name;
	u32		id;
	u8		bit_idx;
	unsigned long	flags;
};

struct clk_hw *sl_clk_register(struct device *dev, void __iomem *reg,
			       const char *name, const char * const *parent_names,
			       unsigned int num_parents, unsigned long flags,
			       spinlock_t *lock);

#endif /* __SL_CLK_H */
