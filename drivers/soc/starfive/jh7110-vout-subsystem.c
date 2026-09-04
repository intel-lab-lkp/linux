// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the StarFive JH7110 video output subsystem
 *
 * Copyright (c) 2025 Samsung Electronics Co., Ltd.
 * Author: Michal Wilczynski <m.wilczynski@samsung.com>
 *
 * The display hardware sits behind a single NoC port whose clock and reset
 * gate access to every register in the region, inside the PD_VOUT power
 * domain. Nothing below this node can reach its own registers until all three
 * are up, so bring them up here and hold them for as long as any child device
 * exists, rather than leaving them to whichever consumer happens to probe
 * first.
 */

#include <linux/clk.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

static void jh7110_vout_subsys_pm_put(void *data)
{
	pm_runtime_put_sync(data);
}

static int jh7110_vout_subsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *bus_rst;
	struct clk *bus_clk;
	int ret;

	/*
	 * Take a runtime PM reference for the lifetime of this device. genpd
	 * only keeps PD_VOUT powered while something actually holds it, and
	 * an unclocked or unpowered access to this region wedges the bus.
	 */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to power on PD_VOUT\n");

	ret = devm_add_action_or_reset(dev, jh7110_vout_subsys_pm_put, dev);
	if (ret)
		return ret;

	bus_clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(bus_clk))
		return dev_err_probe(dev, PTR_ERR(bus_clk),
				     "Failed to enable NoC bus clock\n");

	bus_rst = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(bus_rst))
		return dev_err_probe(dev, PTR_ERR(bus_rst),
				     "Failed to deassert NoC bus reset\n");

	return devm_of_platform_populate(dev);
}

static const struct of_device_id jh7110_vout_subsys_of_match[] = {
	{ .compatible = "starfive,jh7110-vout-subsystem", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jh7110_vout_subsys_of_match);

static struct platform_driver jh7110_vout_subsys_driver = {
	.probe = jh7110_vout_subsys_probe,
	.driver = {
		.name = "jh7110-vout-subsystem",
		.of_match_table = jh7110_vout_subsys_of_match,
	},
};
module_platform_driver(jh7110_vout_subsys_driver);

MODULE_AUTHOR("Michal Wilczynski <m.wilczynski@samsung.com>");
MODULE_DESCRIPTION("StarFive JH7110 video output subsystem driver");
MODULE_LICENSE("GPL");
