// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Silicon hardware tunable support
 *
 * Each tunable is a list with each entry containing a offset into the MMIO
 * region, a mask of bits to be cleared and a set of bits to be set. These
 * tunables are passed along by the previous boot stages and vary from device
 * to device such that they cannot be hardcoded in the individual drivers.
 *
 * Copyright (C) The Asahi Linux Contributors
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/apple/tunable.h>

struct apple_tunable *devm_apple_tunable_parse(struct device *dev,
					       struct device_node *np,
					       const char *name)
{
	struct apple_tunable *tunable;
	struct property *prop;
	const __be32 *p;
	size_t sz;
	int i;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return ERR_PTR(-ENOENT);

	if (prop->length % (3 * sizeof(u32)))
		return ERR_PTR(-EINVAL);
	sz = prop->length / (3 * sizeof(u32));

	tunable = devm_kzalloc(dev,
			       sizeof(*tunable) + sz * sizeof(*tunable->values),
			       GFP_KERNEL);
	if (!tunable)
		return ERR_PTR(-ENOMEM);
	tunable->sz = sz;

	for (i = 0, p = NULL; i < tunable->sz; ++i) {
		p = of_prop_next_u32(prop, p, &tunable->values[i].offset);
		p = of_prop_next_u32(prop, p, &tunable->values[i].mask);
		p = of_prop_next_u32(prop, p, &tunable->values[i].value);
	}

	return tunable;
}
EXPORT_SYMBOL(devm_apple_tunable_parse);

void apple_tunable_apply(void __iomem *regs, struct apple_tunable *tunable)
{
	size_t i;

	for (i = 0; i < tunable->sz; ++i) {
		u32 val, old_val;

		val = old_val = readl_relaxed(regs + tunable->values[i].offset);
		val &= ~tunable->values[i].mask;
		val |= tunable->values[i].value;
		if (val != old_val)
			writel_relaxed(val, regs + tunable->values[i].offset);
	}
}
EXPORT_SYMBOL(apple_tunable_apply);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Sven Peter <sven@kernel.org>");
MODULE_DESCRIPTION("Apple Silicon hardware tunable support");
