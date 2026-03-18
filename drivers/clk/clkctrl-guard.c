// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
/*
 * Clock Controller Guard Driver
 *
 * Copyright 2026 Bruker Corporation
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>

#define MAX_INPUT_GPIO_COUNT 32

/**
 * struct clkctrl_guard_priv - private state for the whole controller
 * @dev:		platform device
 *
 * @clks:		array of input clock descriptors
 * @num_clks:		number of entries in @inputs
 *
 * @gpios:		array of GPIO descriptors
 * @gpio_names:		GPIO names
 * @num_gpios:		number of input GPIOs
 *
 * @output_hw_clk:      output clock HW descriptor
 * @output_clock_name:  output clock name
 */
struct clkctrl_guard_priv {
	struct device *dev;

	struct clk_bulk_data *clks;
	int num_clks;

	struct gpio_descs *gpios;
	const char **gpio_names;
	int num_gpios;

	struct clk_hw output_hw_clk;
	const char *output_clock_name;
};

#define to_clkctrl_guard_priv(_hw) \
	container_of(_hw, struct clkctrl_guard_priv, output_hw_clk)

static int clkctrl_guard_enable(struct clk_hw *hw)
{
	struct clkctrl_guard_priv *priv = to_clkctrl_guard_priv(hw);

	dev_dbg(priv->dev, "enable output clk '%s'\n", priv->output_clock_name);

	return clk_bulk_enable(priv->num_clks, priv->clks);
}

static void clkctrl_guard_disable(struct clk_hw *hw)
{
	struct clkctrl_guard_priv *priv = to_clkctrl_guard_priv(hw);

	dev_dbg(priv->dev, "disable output clk '%s'\n", priv->output_clock_name);

	clk_bulk_disable(priv->num_clks, priv->clks);
}

static int clkctrl_guard_prepare(struct clk_hw *hw)
{
	struct clkctrl_guard_priv *priv = to_clkctrl_guard_priv(hw);

	return clk_bulk_prepare(priv->num_clks, priv->clks);
}

static void clkctrl_guard_unprepare(struct clk_hw *hw)
{
	struct clkctrl_guard_priv *priv = to_clkctrl_guard_priv(hw);

	clk_bulk_unprepare(priv->num_clks, priv->clks);
}

static int is_gpio_ready(struct clkctrl_guard_priv *priv)
{
	unsigned long values[BITS_TO_LONGS(MAX_INPUT_GPIO_COUNT)];
	int ret = 0;

	if (priv->num_gpios == 0)
		return 0;

	ret = gpiod_get_array_value(priv->gpios->ndescs,
		priv->gpios->desc,
		priv->gpios->info,
		values);

	if (ret) {
		dev_err(priv->dev, "Failed to read GPIOs");
		return -EIO;
	}

	for (int i = 0; i < priv->gpios->ndescs; i++) {
		if (!test_bit(i, values)) {
			dev_warn(priv->dev, "GPIO %s is not ready", priv->gpio_names[i]);
			return -EBUSY;
		}
	}

	return 0;
}

static int clkctrl_guard_is_prepared(struct clk_hw *hw)
{
	struct clkctrl_guard_priv *priv = to_clkctrl_guard_priv(hw);
	int ret = 0;

	if (priv->num_gpios > 0) {
		ret = is_gpio_ready(priv);
		if (ret < 0)
			return ret;
	}

	// Now check for the clocks
	for (int i = 0; i < priv->num_clks; i++) {
		struct clk_hw *hw_clk = __clk_get_hw(priv->clks[i].clk);

		if (!clk_hw_is_prepared(hw_clk)) {
			dev_dbg(priv->dev, "Clock %i (%s) is not ready",
				i, priv->clks[i].id);
			return -EBUSY;
		}
	}

	return 0;
}

/* We have to implement it, but we are not going to control
 * parent clock selection
 */
static u8 clkctrl_guard_get_parent(struct clk_hw *hw)
{
	return 0;
}

static const struct clk_ops clkctrl_guard_ops = {
	.enable =	clkctrl_guard_enable,
	.disable =	clkctrl_guard_disable,
	.prepare =	clkctrl_guard_prepare,
	.unprepare =	clkctrl_guard_unprepare,
	.is_prepared =	clkctrl_guard_is_prepared,
	.get_parent =	clkctrl_guard_get_parent,
};

static int clkctrl_guard_parse_inputs(struct clkctrl_guard_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0) {
		dev_err(dev, "failed to get input clocks: %d\n", ret);
		return ret ? ret : -ENOENT;
	}

	priv->num_clks = ret;

	if (priv->num_clks == 0)
		dev_info(dev, "No input clocks provided\n");

	for (int i = 0; i < priv->num_clks; i++)
		dev_dbg(dev, "input clk[%d]: name='%s' rate=%lu Hz\n",
			i, priv->clks[i].id,
			clk_get_rate(priv->clks[i].clk));

	return 0;
}

static int clkctrl_guard_parse_gpios(struct clkctrl_guard_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *np = dev->of_node;
	int i;

	priv->gpios = devm_gpiod_get_array_optional(dev, NULL, GPIOD_ASIS);
	if (IS_ERR(priv->gpios)) {
		dev_err(dev, "failed to get GPIO array: %ld\n",
				PTR_ERR(priv->gpios));
		return PTR_ERR(priv->gpios);
	}

	if (!priv->gpios) {
		dev_info(dev, "No GPIOs provided, continue\n");
		priv->num_gpios = 0;
		return 0;
	}

	priv->num_gpios = priv->gpios->ndescs;
	if (priv->num_gpios > 32) {
		dev_err(priv->dev, "Maximum number of input GPIOs is 32\n");
		return -EINVAL;
	}

	/* gpio_descs carries no names, so read "gpio-names" separately */
	priv->gpio_names = devm_kcalloc(dev, priv->num_gpios, sizeof(*priv->gpio_names),
			GFP_KERNEL);
	if (!priv->gpio_names)
		return -ENOMEM;

	for (i = 0; i < priv->num_gpios; i++) {
		of_property_read_string_index(np, "gpio-names", i,
				&priv->gpio_names[i]);

		dev_dbg(dev, "gpio[%d]: name='%s'\n",
				i, priv->gpio_names[i] ? priv->gpio_names[i] : "(unnamed)");
	}

	return 0;
}

static int clkctrl_guard_parse_outputs(struct clkctrl_guard_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *np = dev->of_node;
	struct clk_init_data init = {};
	int ret;

	of_property_read_string_index(np, "clock-output-names", 0,
		&priv->output_clock_name);

	if (!priv->output_clock_name)
		priv->output_clock_name = dev_name(priv->dev);

	init.name = priv->output_clock_name;
	init.ops = &clkctrl_guard_ops;
	init.flags = 0;
	init.num_parents = priv->num_clks;

	if (priv->num_clks) {
		const char **parent_names;
		int j;

		parent_names = devm_kcalloc(dev, priv->num_clks,
					    sizeof(*parent_names),
					    GFP_KERNEL);
		if (!parent_names)
			return -ENOMEM;

		for (j = 0; j < priv->num_clks; j++)
			parent_names[j] = priv->clks[j].id;

		init.parent_names = parent_names;
	}

	priv->output_hw_clk.init = &init;

	ret = devm_clk_hw_register(dev, &priv->output_hw_clk);
	if (ret) {
		dev_err(dev, "failed to register output clk'%s': %d\n",
			priv->output_clock_name, ret);
		return ret;
	}

	dev_info(priv->dev, "Output clock '%s' registered\n", priv->output_clock_name);

	return 0;
}

static int clkctrl_guard_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clkctrl_guard_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	ret = clkctrl_guard_parse_inputs(priv);
	if (ret)
		return ret;

	ret = clkctrl_guard_parse_gpios(priv);
	if (ret)
		return ret;

	if (priv->num_clks == 0 && priv->num_gpios == 0) {
		dev_err(priv->dev, "At least 1 input clock or input GPIO is required\n");
		return -EINVAL;
	}

	ret = clkctrl_guard_parse_outputs(priv);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(priv->dev, of_clk_hw_simple_get,
		&priv->output_hw_clk);
	if (ret) {
		dev_err(priv->dev, "failed to register clock provider '%s': %d\n",
			priv->output_clock_name, ret);
		return ret;
	}

	dev_info(dev, "registered %u input clocks, %u GPIOs\n",
		 priv->num_clks, priv->num_gpios);

	return 0;
}

static void clkctrl_guard_remove(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "removed\n");
}

static const struct of_device_id clkctrl_guard_of_match[] = {
	{ .compatible = "clock-controller-guard" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, clkctrl_guard_of_match);

static struct platform_driver clkctrl_guard_driver = {
	.probe  = clkctrl_guard_probe,
	.remove = clkctrl_guard_remove,
	.driver = {
		.name           = "clock-controller-guard",
		.of_match_table = clkctrl_guard_of_match,
	},
};
module_platform_driver(clkctrl_guard_driver);

MODULE_AUTHOR("Vyacheslav Yurkov <V.Yurkov.EXT@bruker.com>");
MODULE_DESCRIPTION("GPIO-controlled clock controller driver");
MODULE_LICENSE("Dual BSD/GPL");
