// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#include <linux/platform_device.h>
#include <linux/auxiliary_bus.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/io.h>

#include "clk-zx.h"

static void zx_adev_release(struct device *dev)
{
	dev_info(dev, "Aux device released.\n");
}

static void zx_adev_unregister(void *data)
{
	struct auxiliary_device *adev = data;

	auxiliary_device_delete(adev);
	auxiliary_device_uninit(adev);
}

int zx_clk_probe(struct platform_device *pdev)
{
	unsigned int public_clk_count = 1, highest_id = 0;
	struct clk_hw_onecell_data *clocks;
	struct device *dev = &pdev->dev;
	const struct zx_clk_data *data;
	struct auxiliary_device *adev;
	struct regmap *map;
	struct clk *clk;
	unsigned int i;
	int res;

	data = device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	map = device_node_to_regmap(dev->of_node);
	if (!map)
		return -EINVAL;

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
		clk = devm_clk_get_enabled(dev, data->inputs_enable[i]);
		if (IS_ERR(clk)) {
			return dev_err_probe(dev, PTR_ERR(clk), "Input clk %s failure\n",
					     data->inputs_enable[i]);
		}
	}
	for (i = 0; i < data->num_inputs; ++i) {
		clk = devm_clk_get(dev, data->inputs[i]);
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

	res = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
	if (res)
		return res;

	adev = devm_kzalloc(dev, sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;

	adev->name = data->reset_auxdev_name;
	adev->dev.parent = dev;
	adev->dev.release = zx_adev_release;
	adev->dev.of_node = dev->of_node;

	res = auxiliary_device_init(adev);
	if (res)
		return dev_err_probe(dev, res, "Failed to init aux dev %s\n", adev->name);

	res = auxiliary_device_add(adev);
	if (res) {
		auxiliary_device_uninit(adev);
		return dev_err_probe(dev, res, "Failed to add aux dev %s\n", adev->name);
	}

	return devm_add_action_or_reset(dev, zx_adev_unregister, adev);
}
EXPORT_SYMBOL_NS_GPL(zx_clk_probe, "ZTE_CLK");

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE common clock driver");
MODULE_LICENSE("GPL");
