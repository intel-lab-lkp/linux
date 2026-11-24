// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 * Copyright (c) 2026, Artur Kowalski. All rights reserved.
 */

#include <linux/bits.h>
#include <linux/clk/tegra.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/stringify.h>
#include <linux/watchdog.h>

/* Timer registers */
#define TIMER_PTV			0x0
#define TIMER_EN			BIT(31)
#define TIMER_PERIODIC			BIT(30)
#define TIMER_PTV_MAX			GENMASK(28, 0)
#define TIMER_PCR			0x4
#define TIMER_PCR_INTR_CLR		BIT(30)

/*
 * Tegra20 has no watchdog block. Instead, the clock and reset controller can
 * reset the system when the interrupt of timer 1 or 2 fires while the previous
 * one is still pending. The clock driver controls this reset, and this driver
 * uses timer 1.
 */

/* minimum and maximum watchdog trigger timeout, in seconds */
#define MIN_WDT_TIMEOUT			1
/*
 * The reset occurs on the second expiry of the timer, so its period is half
 * of the timeout. The timer has a fixed 1 MHz clock and counts PTV + 1 ticks.
 */
#define MAX_WDT_TIMEOUT		((TIMER_PTV_MAX + 1ull) * 2 / USEC_PER_SEC)

struct tegra20_wdt {
	struct watchdog_device	wdd;
	void __iomem		*tmr_regs;
};

#define WDT_HEARTBEAT 120
static int heartbeat = WDT_HEARTBEAT;
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat,
	"Watchdog heartbeats in seconds. (default = "
	__stringify(WDT_HEARTBEAT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__stringify(WATCHDOG_NOWAYOUT) ")");

static int tegra20_wdt_start(struct watchdog_device *wdd)
{
	struct tegra20_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 period = wdd->timeout * USEC_PER_SEC / 2;

	writel(TIMER_EN | TIMER_PERIODIC | (period - 1),
	       wdt->tmr_regs + TIMER_PTV);

	/*
	 * Clear an interrupt left pending by an earlier use of the timer,
	 * which would otherwise reset the system on the first expiry.
	 */
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);

	return tegra20_clk_watchdog_enable(true);
}

static int tegra20_wdt_stop(struct watchdog_device *wdd)
{
	struct tegra20_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret;

	ret = tegra20_clk_watchdog_enable(false);

	/* stopping the timer stops the watchdog even if the above failed */
	writel(0, wdt->tmr_regs + TIMER_PTV);
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);

	return ret;
}

static int tegra20_wdt_ping(struct watchdog_device *wdd)
{
	struct tegra20_wdt *wdt = watchdog_get_drvdata(wdd);

	/*
	 * Clearing the interrupt alone doesn't restart the timer's period, so
	 * the reset could follow as soon as half the timeout after the ping.
	 * Rewriting the timer setting restarts the period. The interrupt is
	 * cleared afterwards, so one fired in between doesn't count.
	 */
	writel(readl(wdt->tmr_regs + TIMER_PTV), wdt->tmr_regs + TIMER_PTV);
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);

	return 0;
}

static int tegra20_wdt_set_timeout(struct watchdog_device *wdd,
				   unsigned int timeout)
{
	wdd->timeout = timeout;

	/* starting again reprograms the running watchdog */
	if (watchdog_active(wdd))
		return tegra20_wdt_start(wdd);

	return 0;
}

static const struct watchdog_info tegra20_wdt_info = {
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_MAGICCLOSE |
			  WDIOF_KEEPALIVEPING,
	.firmware_version = 0,
	.identity	= "Tegra20 Watchdog",
};

static const struct watchdog_ops tegra20_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	tegra20_wdt_start,
	.stop =		tegra20_wdt_stop,
	.ping =		tegra20_wdt_ping,
	.set_timeout =	tegra20_wdt_set_timeout,
};

static int tegra20_wdt_init(struct platform_device *pdev,
			    struct tegra20_wdt *wdt,
			    void __iomem *regs)
{
	struct device *dev = &pdev->dev;
	int ret;

	wdt->tmr_regs = regs;

	/*
	 * The bootloader may leave the watchdog running. Take it over with
	 * our own timeout and timer 1, and let the core ping it until user
	 * space opens the device.
	 */
	if (tegra20_clk_watchdog_is_enabled()) {
		dev_info(dev, "watchdog left running by the bootloader, taking it over (timeout %u sec)\n",
			 wdt->wdd.timeout);
		ret = tegra20_wdt_start(&wdt->wdd);
		if (ret)
			return ret;
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	return 0;
}

static int tegra20_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	struct tegra20_wdt *wdt;
	void __iomem *regs;
	int ret;

	/*
	 * Tegra30 lists "nvidia,tegra20-timer" as a fallback, but it has a
	 * dedicated watchdog block, handled by tegra_wdt, and uses TIMER1 as
	 * a per-CPU clock event device, so it isn't free for this one.
	 */
	if (of_device_is_compatible(dev->of_node, "nvidia,tegra30-timer"))
		return -ENODEV;

	/* This is the timer base. */
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	/*
	 * Allocate our watchdog driver data, which has the
	 * struct watchdog_device nested within it.
	 */
	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	/* Initialize struct watchdog_device. */
	wdd = &wdt->wdd;
	wdd->timeout = WDT_HEARTBEAT;
	wdd->info = &tegra20_wdt_info;
	wdd->ops = &tegra20_wdt_ops;
	wdd->min_timeout = MIN_WDT_TIMEOUT;
	wdd->max_timeout = MAX_WDT_TIMEOUT;
	wdd->parent = dev;

	watchdog_init_timeout(wdd, heartbeat, dev);

	watchdog_set_drvdata(wdd, wdt);

	watchdog_set_nowayout(wdd, nowayout);

	/* Initialize struct tegra20_wdt. */
	ret = tegra20_wdt_init(pdev, wdt, regs);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, wdt);

	watchdog_stop_on_unregister(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	dev_info(dev, "initialized (heartbeat = %u sec, nowayout = %d)\n",
		 wdd->timeout, nowayout);

	return 0;
}

/*
 * The watchdog may also run without user space having opened it, when it was
 * taken over from the bootloader, so check whether the hardware runs. Calling
 * the stop operation directly leaves WDOG_HW_RUNNING set for the resume.
 */
static int tegra20_wdt_suspend(struct device *dev)
{
	struct tegra20_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&wdt->wdd))
		tegra20_wdt_stop(&wdt->wdd);

	return 0;
}

static int tegra20_wdt_resume(struct device *dev)
{
	struct tegra20_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&wdt->wdd))
		tegra20_wdt_start(&wdt->wdd);

	return 0;
}

static const struct of_device_id tegra20_wdt_of_match[] = {
	{ .compatible = "nvidia,tegra20-timer" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra20_wdt_of_match);

static DEFINE_SIMPLE_DEV_PM_OPS(tegra20_wdt_pm_ops,
				tegra20_wdt_suspend, tegra20_wdt_resume);

static struct platform_driver tegra20_wdt_driver = {
	.probe		= tegra20_wdt_probe,
	.driver		= {
		.name	= "tegra20-wdt",
		.pm	= pm_sleep_ptr(&tegra20_wdt_pm_ops),
		.of_match_table = tegra20_wdt_of_match,
	},
};
module_platform_driver(tegra20_wdt_driver);

MODULE_AUTHOR("Artur Kowalski");
MODULE_DESCRIPTION("Tegra20 Watchdog Driver");
MODULE_LICENSE("GPL");
