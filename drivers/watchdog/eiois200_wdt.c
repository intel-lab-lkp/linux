// SPDX-License-Identifier: GPL-2.0-only
/*
 * Advantech EIO-IS200 Watchdog Driver
 *
 * Copyright (C) 2023 Advantech Co., Ltd.
 * Author: wenkai <advantech.susiteam@gmail.com>
 */

#include <linux/mfd/core.h>
#include <linux/reboot.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>

#include "../mfd/eiois200.h"

#define WATCHDOG_TIMEOUT	60
#define WATCHDOG_PRETIMEOUT	10

static struct _wdt {
	u32	support;
	long	last_time;
	struct	regmap  *iomap;
	struct	device *dev;
} wdt;

/* Pointer to the eiois200_core device structure */
static struct eiois200_dev *eiois200_dev;

static struct watchdog_info wdinfo = {
	.identity = KBUILD_MODNAME,
	.options  = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
		    WDIOF_MAGICCLOSE,
};

static struct watchdog_device wddev = {
	.info	     = &wdinfo,
	.max_timeout = 0x7FFF,
	.min_timeout = 1,
};

static int wdt_set_timeout(struct watchdog_device *dev,
			   unsigned int _timeout)
{
	dev->timeout = _timeout;
	dev_dbg(wdt.dev, "Set timeout: %d\n", _timeout);

	return 0;
}

static int wdt_start(struct watchdog_device *dev)
{
	return 0;
}

static int wdt_stop(struct watchdog_device *dev)
{
	return 0;
}

static int wdt_ping(struct watchdog_device *dev)
{
	return 0;
}

static unsigned int wdt_get_timeleft(struct watchdog_device *dev)
{
	return 0;
}

static const struct watchdog_ops wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= wdt_start,
	.stop		= wdt_stop,
	.ping		= wdt_ping,
	.set_timeout	= wdt_set_timeout,
	.get_timeleft	= wdt_get_timeleft,
};

static int wdt_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;

	/* Contact eiois200_core */
	eiois200_dev = dev_get_drvdata(dev->parent);
	if (!eiois200_dev)
		return dev_err_probe(dev, ret,
				     "Error contact eiois200_core %d\n", ret);

	wdt.dev = dev;
	wdt.iomap = dev_get_regmap(dev->parent, NULL);
	if (!wdt.iomap)
		return dev_err_probe(dev, -ENOMEM, "Query parent regmap fail\n");

	/* Inform watchdog info */
	wddev.ops = &wdt_ops;
	ret = watchdog_init_timeout(&wddev, wddev.timeout, dev);
	if (ret)
		return dev_err_probe(dev, ret, "Init timeout fail\n");

	watchdog_stop_on_reboot(&wddev);

	watchdog_stop_on_unregister(&wddev);

	/* Register watchdog */
	ret = devm_watchdog_register_device(dev, &wddev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Cannot register watchdog device (err: %d)\n",
				     ret);

	return 0;
}

static struct platform_driver eiois200_wdt_driver = {
	.driver = {
		.name  = "eiois200_wdt",
	},
};
module_platform_driver_probe(eiois200_wdt_driver, wdt_probe);

MODULE_AUTHOR("wenkai <advantech.susiteam@gmail.com>");
MODULE_DESCRIPTION("Watchdog interface for Advantech EIO-IS200 embedded controller");
MODULE_LICENSE("GPL");
