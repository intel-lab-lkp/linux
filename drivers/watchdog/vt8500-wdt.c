// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Alexey Charkov <alchark@gmail.com */

#include <linux/auxiliary_bus.h>
#include <linux/container_of.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/vt8500-timer.h>

static int vt8500_watchdog_start(struct watchdog_device *wdd)
{
	struct vt8500_wdt_info *info = watchdog_get_drvdata(wdd);
	u64 deadline = info->timer_next(wdd->timeout * VT8500_TIMER_HZ);

	writel((u32)deadline, info->wdt_match);
	writel(TIMER_WD_EN, info->wdt_en);
	return 0;
}

static int vt8500_watchdog_stop(struct watchdog_device *wdd)
{
	struct vt8500_wdt_info *info = watchdog_get_drvdata(wdd);

	writel(0, info->wdt_en);
	return 0;
}

static const struct watchdog_ops vt8500_watchdog_ops = {
	.start			= vt8500_watchdog_start,
	.stop			= vt8500_watchdog_stop,
};

static const struct watchdog_info vt8500_watchdog_info = {
	.identity		= "VIA VT8500 watchdog",
	.options		= WDIOF_MAGICCLOSE |
				  WDIOF_KEEPALIVEPING |
				  WDIOF_SETTIMEOUT,
};

static int vt8500_wdt_probe(struct auxiliary_device *auxdev,
			    const struct auxiliary_device_id *id)
{
	struct vt8500_wdt_info *info;
	struct watchdog_device *wdd;

	wdd = devm_kzalloc(&auxdev->dev, sizeof(*wdd), GFP_KERNEL);
	if (!wdd)
		return -ENOMEM;

	wdd->info = &vt8500_watchdog_info;
	wdd->ops = &vt8500_watchdog_ops;
	wdd->max_hw_heartbeat_ms = U32_MAX / (VT8500_TIMER_HZ / 1000);
	wdd->parent = &auxdev->dev;

	info = container_of(auxdev, struct vt8500_wdt_info, auxdev);
	watchdog_set_drvdata(wdd, info);

	return devm_watchdog_register_device(&auxdev->dev, wdd);
}

static const struct auxiliary_device_id vt8500_wdt_ids[] = {
	{ .name = "timer_vt8500.vt8500-wdt" },
	{},
};

MODULE_DEVICE_TABLE(auxiliary, my_auxiliary_id_table);

static struct auxiliary_driver vt8500_wdt_driver = {
	.name =	"vt8500-wdt",
	.probe = vt8500_wdt_probe,
	.id_table = vt8500_wdt_ids,
};
module_auxiliary_driver(vt8500_wdt_driver);

MODULE_AUTHOR("Alexey Charkov <alchark@gmail.com>");
MODULE_DESCRIPTION("Driver for the VIA VT8500 watchdog timer");
MODULE_LICENSE("GPL");
