// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/clocksource.h>
#include <linux/platform_device.h>

extern struct of_device_id __timer_of_table[];

static int __init timer_of_probe(void)
{
	struct device_node *np;
	const struct of_device_id *match;
	of_init_fn_1_ret init_func_ret;
	unsigned timers = 0;
	int ret;

	for_each_matching_node_and_match(np, __timer_of_table, &match) {
		if (!of_device_is_available(np))
			continue;

		init_func_ret = match->data;

		ret = init_func_ret(np);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				pr_err("Failed to initialize '%pOF': %d\n", np,
				       ret);
			continue;
		}

		timers++;
	}

	return timers;
}

static int __init __timer_pdev_probe(struct platform_driver *drv)
{
	struct device_node *np;
	struct platform_device *pdev;
	const struct of_device_id *match;
	unsigned int timers = 0;
	int ret;

	for_each_matching_node_and_match(np, drv->driver.of_match_table, &match) {
		if (!of_device_is_available(np))
			continue;

		pdev = platform_device_alloc(of_node_full_name(np), -1);
		if (!pdev)
			continue;

		ret = device_add_of_node(&pdev->dev, np);
		if (ret) {
			platform_device_put(pdev);
			continue;
		}

		dev_set_name(&pdev->dev, pdev->name);

		ret = drv->probe(pdev);
		if (!ret) {
			timers++;
			continue;
		}

		if (ret != -EPROBE_DEFER)
			pr_err("Failed to initialize '%pOF': %d\n", np, ret);

		device_remove_of_node(&pdev->dev);

		platform_device_put(pdev);
	}

	return timers;
}

static int __init timer_pdev_probe(void)
{
	struct platform_driver **drv;

	for_each_pdev_timer_table(drv)
		__timer_pdev_probe(*drv);

	return 0;
}

void __init timer_probe(void)
{
	unsigned timers = 0;

	timers += timer_of_probe();
	timers += timer_pdev_probe();
	timers += acpi_probe_device_table(timer);

	if (!timers)
		pr_crit("%s: no matching timers found\n", __func__);
}
