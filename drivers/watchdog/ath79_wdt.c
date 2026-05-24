// SPDX-License-Identifier: GPL-2.0-only
/*
 * Atheros AR71XX/AR724X/AR913X built-in hardware watchdog timer.
 *
 * Copyright (C) 2008-2011 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2008 Imre Kaloz <kaloz@openwrt.org>
 *
 * This driver was based on: drivers/watchdog/ixp4xx_wdt.c
 *	Author: Deepak Saxena <dsaxena@plexity.net>
 *	Copyright 2004 (c) MontaVista, Software, Inc.
 *
 * which again was based on sa1100 driver,
 *	Copyright (C) 2000 Oleg Drokin <green@crimea.edu>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_platform.h>

#define DRIVER_NAME	"ath79-wdt"

#define WDT_TIMEOUT	15	/* seconds */

#define WDOG_REG_CTRL		0x00
#define WDOG_REG_TIMER		0x04

#define WDOG_CTRL_LAST_RESET	BIT(31)
#define WDOG_CTRL_ACTION_MASK	3
#define WDOG_CTRL_ACTION_NONE	0	/* no action */
#define WDOG_CTRL_ACTION_GPI	1	/* general purpose interrupt */
#define WDOG_CTRL_ACTION_NMI	2	/* NMI */
#define WDOG_CTRL_ACTION_FCR	3	/* full chip reset */

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started "
			   "(default=" __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static int timeout = WDT_TIMEOUT;
module_param(timeout, int, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds "
			  "(default=" __MODULE_STRING(WDT_TIMEOUT) "s)");

struct ath79_wdt {
	void __iomem *base;
	unsigned long freq;
	struct watchdog_device wdd;
};

static inline void ath79_wdt_wr(struct ath79_wdt *wdt, unsigned int reg, u32 val)
{
	iowrite32(val, wdt->base + reg);
}

static inline u32 ath79_wdt_rr(struct ath79_wdt *wdt, unsigned int reg)
{
	return ioread32(wdt->base + reg);
}

static inline void ath79_wdt_keepalive(struct ath79_wdt *wdt, unsigned int timeout)
{
	ath79_wdt_wr(wdt, WDOG_REG_TIMER, wdt->freq * timeout);
	/* flush write */
	ath79_wdt_rr(wdt, WDOG_REG_TIMER);
}

static inline void ath79_wdt_enable(struct ath79_wdt *wdt, unsigned int timeout)
{
	ath79_wdt_keepalive(wdt, timeout);

	/*
	 * Updating the TIMER register requires a few microseconds
	 * on the AR934x SoCs at least. Use a small delay to ensure
	 * that the TIMER register is updated within the hardware
	 * before enabling the watchdog.
	 */
	udelay(2);

	ath79_wdt_wr(wdt, WDOG_REG_CTRL, WDOG_CTRL_ACTION_FCR);
	/* flush write */
	ath79_wdt_rr(wdt, WDOG_REG_CTRL);
}

static inline void ath79_wdt_disable(struct ath79_wdt *wdt)
{
	ath79_wdt_wr(wdt, WDOG_REG_CTRL, WDOG_CTRL_ACTION_NONE);
	/* flush write */
	ath79_wdt_rr(wdt, WDOG_REG_CTRL);
}

static int ath79_wdt_ping(struct watchdog_device *wdd)
{
	struct ath79_wdt *wdt = watchdog_get_drvdata(wdd);

	ath79_wdt_keepalive(wdt, wdd->timeout);

	return 0;
}

static int ath79_wdt_set_timeout(struct watchdog_device *wdd, unsigned int val)
{
	struct ath79_wdt *wdt = watchdog_get_drvdata(wdd);

	wdd->timeout = val;
	ath79_wdt_keepalive(wdt, val);

	return 0;
}

static int ath79_wdt_start(struct watchdog_device *wdd)
{
	struct ath79_wdt *wdt = watchdog_get_drvdata(wdd);

	ath79_wdt_enable(wdt, wdd->timeout);

	return 0;
}

static unsigned int ath79_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct ath79_wdt *wdt = watchdog_get_drvdata(wdd);

	return ath79_wdt_rr(wdt, WDOG_REG_TIMER) / wdt->freq;
}

static int ath79_wdt_stop(struct watchdog_device *wdd)
{
	struct ath79_wdt *wdt = watchdog_get_drvdata(wdd);

	ath79_wdt_disable(wdt);

	return 0;
}

static const struct watchdog_info ath79_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_CARDRESET | WDIOF_MAGICCLOSE,
	.firmware_version = 0,
	.identity = "ATH79 watchdog",
};

static const struct watchdog_ops ath79_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ath79_wdt_start,
	.stop = ath79_wdt_stop,
	.ping = ath79_wdt_ping,
	.set_timeout = ath79_wdt_set_timeout,
	.get_timeleft = ath79_wdt_get_timeleft,
};

static int ath79_wdt_probe(struct platform_device *pdev)
{
	struct ath79_wdt *wdt;
	struct clk *wdt_clk;
	u32 ctrl;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	wdt_clk = devm_clk_get_enabled(&pdev->dev, "wdt");
	if (IS_ERR(wdt_clk))
		return PTR_ERR(wdt_clk);

	wdt->freq = clk_get_rate(wdt_clk);
	if (!wdt->freq)
		return -EINVAL;

	ctrl = ath79_wdt_rr(wdt, WDOG_REG_CTRL);
	wdt->wdd.bootstatus = (ctrl & WDOG_CTRL_LAST_RESET) ? WDIOF_CARDRESET : 0;

	wdt->wdd.parent = &pdev->dev;
	wdt->wdd.info = &ath79_wdt_info;
	wdt->wdd.ops = &ath79_wdt_ops;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.timeout = WDT_TIMEOUT;
	wdt->wdd.max_timeout = U32_MAX / wdt->freq;
	watchdog_init_timeout(&wdt->wdd, timeout, &pdev->dev);
	watchdog_set_nowayout(&wdt->wdd, nowayout);
	watchdog_stop_on_reboot(&wdt->wdd);

	watchdog_set_drvdata(&wdt->wdd, wdt);

	return devm_watchdog_register_device(&pdev->dev, &wdt->wdd);
}

static const struct of_device_id ath79_wdt_match[] = {
	{ .compatible = "qca,ar7130-wdt" },
	{},
};
MODULE_DEVICE_TABLE(of, ath79_wdt_match);

static struct platform_driver ath79_wdt_driver = {
	.probe		= ath79_wdt_probe,
	.driver		= {
		.name	= DRIVER_NAME,
		.of_match_table = ath79_wdt_match,
	},
};

module_platform_driver(ath79_wdt_driver);

MODULE_DESCRIPTION("Atheros AR71XX/AR724X/AR913X hardware watchdog driver");
MODULE_AUTHOR("Gabor Juhos <juhosg@openwrt.org>");
MODULE_AUTHOR("Imre Kaloz <kaloz@openwrt.org>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" DRIVER_NAME);
