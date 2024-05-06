// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/slab.h>

#include <soc/tegra/tegra-cfg.h>

static int tegra_cfg_update_reg_info(struct device_node *cfg_node,
				     const struct tegra_cfg_field_desc *field,
				     struct tegra_cfg_reg *regs,
				     struct tegra_cfg *cfg)
{
	int ret;
	unsigned int k, value = 0;

	ret = of_property_read_u32(cfg_node, field->name, &value);
	if (ret)
		return ret;

	/*
	 * Find matching register for this field in register info. Field info
	 * has details of register offset.
	 */
	for (k = 0; k < cfg->num_regs; ++k) {
		if (regs[k].offset == field->offset)
			break;
	}

	/* If register not found, add new at end of list */
	if (k == cfg->num_regs) {
		cfg->num_regs++;
		regs[k].offset = field->offset;
	}

	/* add field value to register */
	value = value << __ffs(field->mask);
	regs[k].value |= value & field->mask;
	regs[k].mask |= field->mask;

	return 0;
}

/*
 * Initialize config list. Parse config node for properties (register fields).
 * Get list of configs and value of fields populated in tegra_cfg_desc.
 * Consolidate field data in reg offset, mask & value format in tegra_cfg.
 * Repeat for each config and store in tegra_cfg_list.
 */
static struct tegra_cfg_list *tegra_cfg_init(struct device *dev,
					     const struct device_node *np,
					     const struct tegra_cfg_desc *cfg_desc)
{
	struct device_node *np_cfg = NULL, *child;
	struct tegra_cfg_reg *regs;
	struct tegra_cfg_list *list;
	struct tegra_cfg *cfg;
	const struct tegra_cfg_field_desc *fields;
	unsigned int count, i;
	int ret;

	if (np)
		np_cfg = of_get_child_by_name(np, "config");
	if (!np_cfg)
		return ERR_PTR(-ENODEV);

	count = of_get_child_count(np_cfg);
	if (count <= 0) {
		dev_dbg(dev, "Node %s: No child node for config settings\n",
			np->name);
		return ERR_PTR(-ENODEV);
	}

	list = devm_kzalloc(dev, sizeof(*list), GFP_KERNEL);
	if (!list)
		return ERR_PTR(-ENOMEM);
	list->num_cfg = 0;
	list->cfg = NULL;

	/* allocate mem for all configurations */
	list->cfg = devm_kcalloc(dev, count, sizeof(*list->cfg),
				 GFP_KERNEL);
	if (!list->cfg)
		return ERR_PTR(-ENOMEM);

	fields = cfg_desc->fields;
	count = 0;
	/*
	 * Iterate through all configurations.
	 */
	for_each_available_child_of_node(np_cfg, child) {
		cfg = &list->cfg[count];

		regs = devm_kcalloc(dev, cfg_desc->num_regs,
				    sizeof(*regs), GFP_KERNEL);
		if (!regs)
			return ERR_PTR(-ENOMEM);

		cfg->name = child->name;
		cfg->regs = regs;
		cfg->num_regs = 0;

		/* Look for all fields in 'child' config */
		for (i = 0; i < cfg_desc->num_fields; i++) {
			ret = tegra_cfg_update_reg_info(child, &fields[i],
							regs, cfg);
			if (ret < 0)
				continue;
		}
		count++;
	}

	list->num_cfg = count;

	return list;
}

struct tegra_cfg *
tegra_cfg_get_by_name(struct device *dev,
		      const struct tegra_cfg_list *list,
		      const char *name)
{
	unsigned int i;

	for (i = 0; i < list->num_cfg; ++i) {
		if (!strcmp(list->cfg[i].name, name))
			return &list->cfg[i];
	}

	return NULL;
}
EXPORT_SYMBOL(tegra_cfg_get_by_name);

struct tegra_cfg_list *tegra_cfg_get(struct device *dev,
				     const struct device_node *np,
				     const struct tegra_cfg_desc *cfg_desc)
{
	return tegra_cfg_init(dev, np, cfg_desc);
}
EXPORT_SYMBOL(tegra_cfg_get);
