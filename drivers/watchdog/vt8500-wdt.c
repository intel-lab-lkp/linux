// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2025 Alexey Charkov <alchark@gmail.com */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/watchdog.h>

#define TIMER_MATCH_REG(x)	(4 * (x))
#define TIMER_COUNT_REG		0x0010	 /* clocksource counter */

#define TIMER_WATCHDOG_EN_REG	0x0018
#define TIMER_WD_EN		BIT(0)

#define TIMER_CTRL_REG		0x0020
#define TIMER_CTRL_ENABLE	BIT(0)	 /* enable clocksource counter */
#define TIMER_CTRL_RD_REQ	BIT(1)	 /* request counter read */

#define TIMER_ACC_STS_REG	0x0024	 /* access status */
#define TIMER_ACC_WR_MATCH(x)	BIT((x)) /* writing Match (x) value */
#define TIMER_ACC_WR_COUNTER	BIT(4)	 /* writing clocksource counter */
#define TIMER_ACC_RD_COUNTER	BIT(5)	 /* reading clocksource counter */

#define VT8500_TIMER_HZ		3000000
#define msecs_to_loops(t) (loops_per_jiffy / 1000 * HZ * t)

struct vt8500_wdt {
	void __iomem *regbase;
	struct watchdog_device wdd;
};

static u64 vt8500_timer_read(void __iomem *regbase)
{
	int loops = msecs_to_loops(10);

	writel(TIMER_CTRL_ENABLE | TIMER_CTRL_RD_REQ, regbase + TIMER_CTRL_REG);
	while (readl(regbase + TIMER_ACC_STS_REG) & TIMER_ACC_RD_COUNTER
	     && --loops)
		cpu_relax();
	return readl(regbase + TIMER_COUNT_REG);
}

static int vt8500_watchdog_start(struct watchdog_device *wdd)
{
	struct vt8500_wdt *drvdata = watchdog_get_drvdata(wdd);
	u64 cycles = wdd->timeout * VT8500_TIMER_HZ;
	u64 deadline = vt8500_timer_read(drvdata->regbase) + cycles;

	writel((u32)deadline, drvdata->regbase + TIMER_MATCH_REG(0));
	writel(TIMER_WD_EN, drvdata->regbase + TIMER_WATCHDOG_EN_REG);
	return 0;
}

static int vt8500_watchdog_stop(struct watchdog_device *wdd)
{
	struct vt8500_wdt *drvdata = watchdog_get_drvdata(wdd);

	writel(0, drvdata->regbase + TIMER_WATCHDOG_EN_REG);
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

static int vt8500_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vt8500_wdt *drvdata;

	drvdata = devm_kzalloc(dev, sizeof(struct vt8500_wdt), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	/*
	 * The register area where the timer and watchdog reside is disarranged.
	 * Hence mapping individual register blocks for the timer and watchdog
	 * is not recommended as they would have access to each others
	 * registers. The timer driver creates the watchdog as a child device.
	 * During the watchdogs creation, the timer driver passes the base
	 * address to the watchdog over the private interface.
	 */

	drvdata->regbase = (void __iomem *)dev->platform_data;

	drvdata->wdd.info = &vt8500_watchdog_info;
	drvdata->wdd.ops = &vt8500_watchdog_ops;
	drvdata->wdd.max_hw_heartbeat_ms = -1UL / (VT8500_TIMER_HZ / 1000);
	drvdata->wdd.parent = dev;

	watchdog_set_drvdata(&drvdata->wdd, drvdata);

	return devm_watchdog_register_device(dev, &drvdata->wdd);
}

static struct platform_driver vt8500_wdt_driver = {
	.probe = vt8500_wdt_probe,
	.driver = {
		.name =	"vt8500-wdt",
	},
};
module_platform_driver(vt8500_wdt_driver);

MODULE_AUTHOR("Alexey Charkov <alchark@gmail.com>");
MODULE_DESCRIPTION("Driver for the VIA VT8500 watchdog timer");
MODULE_LICENSE("GPL");
