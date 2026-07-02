// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/err.h>

#include "clk-zx.h"

static void zx_delete_clk_provider(void *data)
{
	of_clk_del_provider(data);
}

static void zx_clk_disable_unprepare_put(void *data)
{
	clk_disable_unprepare(data);
	clk_put(data);
}

int zx_clk_common_probe(struct device *dev, struct device_node *of_node,
			const struct zx_clk_data *data)
{
	unsigned int public_clk_count = 1, highest_id = 0;
	struct clk_hw_onecell_data *clocks;
	struct regmap *map;
	struct clk *clk;
	unsigned int i;
	int res;

	map = device_node_to_regmap(of_node);
	if (IS_ERR(map))
		return PTR_ERR(map);

	for (i = 0; i < data->num_plls; ++i) {
		if (data->plls[i].id) {
			unsigned int last_idx = data->plls[i].id + data->plls[i].num_postdivs - 1;

			if (last_idx > highest_id)
				highest_id = last_idx;
			public_clk_count += data->plls[i].num_postdivs;
		}
	}
	for (i = 0; i < data->num_muxes; ++i) {
		if (data->muxes[i].id) {
			if (data->muxes[i].id > highest_id)
				highest_id = data->muxes[i].id;
			public_clk_count++;
		}
	}
	for (i = 0; i < data->num_divs; ++i) {
		if (data->divs[i].id) {
			if (data->divs[i].id > highest_id)
				highest_id = data->divs[i].id;
			public_clk_count++;
		}
	}
	for (i = 0; i < data->num_gates; ++i) {
		if (data->gates[i].id) {
			if (data->gates[i].id > highest_id)
				highest_id = data->gates[i].id;
			public_clk_count++;
		}
	}

	if (WARN_ON(public_clk_count != highest_id + 1))
		return -EINVAL;

	clocks = devm_kzalloc(dev, struct_size(clocks, hws, public_clk_count), GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks->num = public_clk_count;

	for (i = 0; i < data->num_inputs_enable; ++i) {
		clk = of_clk_get_by_name(of_node, data->inputs_enable[i]);
		if (IS_ERR(clk)) {
			return dev_err_probe(dev, PTR_ERR(clk), "Input clk %s failure\n",
					     data->inputs_enable[i]);
		}

		res = clk_prepare_enable(clk);
		if (res) {
			clk_put(clk);
			return dev_err_probe(dev, res, "Input clk %s enable failure\n",
					     data->inputs_enable[i]);
		}
		res = devm_add_action_or_reset(dev, zx_clk_disable_unprepare_put, clk);
		if (res)
			return res;
	}
	for (i = 0; i < data->num_inputs; ++i) {
		/* FIXME: devm_get_clk_from_child doesn't do any tree traversal, so it works here
		 * whether "of_node" belongs to "dev" or a parent of "dev". Is it supposed to be
		 * used that way though?
		 */
		clk = devm_get_clk_from_child(dev, of_node, data->inputs[i]);
		if (IS_ERR(clk)) {
			return dev_err_probe(dev, PTR_ERR(clk), "Input clk %s failure\n",
					     data->inputs[i]);
		}
	}

	res = zx_clk_register_plls(dev, map, data->plls, data->num_plls, clocks);
	if (res)
		return res;

	res = zx_clk_register_muxes(dev, map, data->muxes, data->num_muxes, clocks);
	if (res)
		return res;

	res = zx_clk_register_dividers(dev, map, data->divs, data->num_divs, clocks);
	if (res)
		return res;

	res = zx_clk_register_gates(dev, map, data->gates, data->num_gates, clocks);
	if (res)
		return res;

	/* This is to catch holes in the tables rather than registration errors. The count vs
	 * highest ID should catch most static issues. This check here will trigger if an ID is
	 * reused by accident.
	 */
	for (i = 1; i < public_clk_count; i++) {
		if (WARN(!clocks->hws[i], "Clock %u not registered\n", i))
			return -EINVAL;
	}

	res = of_clk_add_hw_provider(of_node, of_clk_hw_onecell_get, clocks);
	if (res)
		return res;
	return devm_add_action_or_reset(dev, zx_delete_clk_provider, of_node);
}
EXPORT_SYMBOL_NS_GPL(zx_clk_common_probe, "ZTE_CLK");

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE common clock driver");
MODULE_LICENSE("GPL");
