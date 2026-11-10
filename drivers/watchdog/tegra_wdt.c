// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/stringify.h>
#include <linux/watchdog.h>

#include <soc/tegra/pmc.h>

/* minimum and maximum watchdog trigger timeout, in seconds */
#define MIN_WDT_TIMEOUT			1
#define MAX_WDT_TIMEOUT			255

/*
 * Base of the WDT registers, from the timer base address.  There are
 * actually 5 watchdogs that can be configured (by pairing with an available
 * timer), at bases 0x100 + (WDT ID) * 0x20, where WDT ID is 0 through 4.
 * This driver only configures the first watchdog (WDT ID 0).
 */
#define WDT_BASE			0x100
#define WDT_ID				0

/*
 * Register base of the timer that's selected for pairing with the watchdog.
 * This driver arbitrarily uses timer 5, which is currently unused by
 * other drivers (in particular, the Tegra clocksource driver).  If this
 * needs to change, take care that the new timer is not used by the
 * clocksource driver.
 */
#define WDT_TIMER_BASE			0x60
#define WDT_TIMER_ID			5

/* WDT registers */
#define WDT_CFG				0x0
#define WDT_CFG_PERIOD_SHIFT		4
#define WDT_CFG_PERIOD_MASK		0xff
#define WDT_CFG_INT_EN			(1 << 12)
#define WDT_CFG_PMC2CAR_RST_EN		(1 << 15)
#define WDT_STS				0x4
#define WDT_STS_COUNT_SHIFT		4
#define WDT_STS_COUNT_MASK		0xff
#define WDT_STS_EXP_SHIFT		12
#define WDT_STS_EXP_MASK		0x3
#define WDT_CMD				0x8
#define WDT_CMD_START_COUNTER		(1 << 0)
#define WDT_CMD_DISABLE_COUNTER		(1 << 1)
#define WDT_UNLOCK			(0xc)
#define WDT_UNLOCK_PATTERN		(0xc45a << 0)

/* Timer registers */
#define TIMER_PTV			0x0
#define TIMER_EN			(1 << 31)
#define TIMER_PERIODIC			(1 << 30)
#define TIMER_PTV_MAX			GENMASK(28, 0)
#define TIMER_PCR			0x4
#define TIMER_PCR_INTR_CLR		(1 << 30)

/*
 * Tegra20 has no watchdog block. Instead, the clock and reset controller can
 * reset the system when the interrupt of timer 1 or 2 fires while the previous
 * one is still pending. This is controlled by its RST_SOURCE register.
 */
#define T20_WDT_TIMER_BASE		0x0
#define T20_RST_SOURCE			0x0
#define T20_RST_SOURCE_WDT_EN		(1 << 5)
#define T20_RST_SOURCE_WDT_SEL_TMR2	(1 << 4)
#define T20_RST_SOURCE_WDT_SYS_RST_EN	(1 << 2)
#define T20_RST_SOURCE_WDT_MASK		(T20_RST_SOURCE_WDT_EN | \
					 T20_RST_SOURCE_WDT_SEL_TMR2 | \
					 T20_RST_SOURCE_WDT_SYS_RST_EN)

/*
 * The reset occurs on the second expiry of the timer, so its period is half
 * of the timeout. The timer has a fixed 1 MHz clock and counts PTV + 1 ticks.
 */
#define T20_MAX_WDT_TIMEOUT		((TIMER_PTV_MAX + 1ull) * 2 / USEC_PER_SEC)

struct tegra_wdt {
	struct watchdog_device	wdd;
	void __iomem		*wdt_regs;
	void __iomem		*tmr_regs;

	/* Tegra20 only */
	void __iomem		*rst_source;
	struct notifier_block	powergate_nb;
	struct mutex		lock;		/* hardware and vde_off */
	bool			vde_off;
};

struct tegra_wdt_soc {
	const struct watchdog_ops *ops;
	unsigned int max_timeout;
	int (*init)(struct platform_device *pdev, struct tegra_wdt *wdt,
		    void __iomem *regs);
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

/*
 * After a watchdog reset, Tegra20's boot ROM hangs if the VDE partition is
 * powered off, which the PMC driver does when it registers the partition,
 * unless booted with tegra_pmc.vde_always_on=1. By default, the watchdog is
 * therefore only used until the partition is powered off for the first time,
 * which still allows a watchdog started by the bootloader to catch hangs early
 * during boot. tegra_pmc.vde_always_on=1 keeps the watchdog usable for good.
 * Clearing this keeps it usable as well, but at the risk of the system
 * hanging instead of rebooting.
 */
static bool stop_on_vde_off = true;
module_param(stop_on_vde_off, bool, 0);
MODULE_PARM_DESC(stop_on_vde_off,
	"Tegra20: stop the watchdog for good when the VDE partition is powered off (default=1)");

static int tegra_wdt_start(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;

	/*
	 * This thing has a fixed 1MHz clock.  Normally, we would set the
	 * period to 1 second by writing 1000000ul, but the watchdog system
	 * reset actually occurs on the 4th expiration of this counter,
	 * so we set the period to 1/4 of this amount.
	 */
	val = 1000000ul / 4;
	val |= (TIMER_EN | TIMER_PERIODIC);
	writel(val, wdt->tmr_regs + TIMER_PTV);

	/*
	 * Set number of periods and start counter.
	 *
	 * Interrupt handler is not required for user space
	 * WDT accesses, since the caller is responsible to ping the
	 * WDT to reset the counter before expiration, through ioctls.
	 */
	val = WDT_TIMER_ID |
	      (wdd->timeout << WDT_CFG_PERIOD_SHIFT) |
	      WDT_CFG_PMC2CAR_RST_EN;
	writel(val, wdt->wdt_regs + WDT_CFG);

	writel(WDT_CMD_START_COUNTER, wdt->wdt_regs + WDT_CMD);

	return 0;
}

static int tegra_wdt_stop(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(WDT_UNLOCK_PATTERN, wdt->wdt_regs + WDT_UNLOCK);
	writel(WDT_CMD_DISABLE_COUNTER, wdt->wdt_regs + WDT_CMD);
	writel(0, wdt->tmr_regs + TIMER_PTV);

	return 0;
}

static int tegra_wdt_ping(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(WDT_CMD_START_COUNTER, wdt->wdt_regs + WDT_CMD);

	return 0;
}

static int tegra_wdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	wdd->timeout = timeout;

	if (watchdog_active(wdd)) {
		tegra_wdt_stop(wdd);
		return tegra_wdt_start(wdd);
	}

	return 0;
}

static unsigned int tegra_wdt_get_timeleft(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 val;
	int count;
	int exp;

	val = readl(wdt->wdt_regs + WDT_STS);

	/* Current countdown (from timeout) */
	count = (val >> WDT_STS_COUNT_SHIFT) & WDT_STS_COUNT_MASK;

	/* Number of expirations (we are waiting for the 4th expiration) */
	exp = (val >> WDT_STS_EXP_SHIFT) & WDT_STS_EXP_MASK;

	/*
	 * The entire thing is divided by 4 because we are ticking down 4 times
	 * faster due to needing to wait for the 4th expiration.
	 */
	return (((3 - exp) * wdd->timeout) + count) / 4;
}

static const struct watchdog_info tegra_wdt_info = {
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_MAGICCLOSE |
			  WDIOF_KEEPALIVEPING,
	.firmware_version = 0,
	.identity	= "Tegra Watchdog",
};

static const struct watchdog_ops tegra_wdt_ops = {
	.owner = THIS_MODULE,
	.start = tegra_wdt_start,
	.stop = tegra_wdt_stop,
	.ping = tegra_wdt_ping,
	.set_timeout = tegra_wdt_set_timeout,
	.get_timeleft = tegra_wdt_get_timeleft,
};

static int tegra30_wdt_init(struct platform_device *pdev, struct tegra_wdt *wdt,
			    void __iomem *regs)
{
	wdt->wdt_regs = regs + WDT_BASE;
	wdt->tmr_regs = regs + WDT_TIMER_BASE;

	return 0;
}

static void __tegra20_wdt_start(struct tegra_wdt *wdt)
{
	u32 period = wdt->wdd.timeout * USEC_PER_SEC / 2;
	u32 val;

	writel(TIMER_EN | TIMER_PERIODIC | (period - 1),
	       wdt->tmr_regs + TIMER_PTV);

	/*
	 * Clear an interrupt left pending by an earlier use of the timer,
	 * which would otherwise reset the system on the first expiry.
	 */
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);

	val = readl(wdt->rst_source);
	val &= ~T20_RST_SOURCE_WDT_MASK;
	val |= T20_RST_SOURCE_WDT_EN | T20_RST_SOURCE_WDT_SYS_RST_EN;
	writel(val, wdt->rst_source);
}

static void __tegra20_wdt_stop(struct tegra_wdt *wdt)
{
	u32 val;

	val = readl(wdt->rst_source);
	val &= ~T20_RST_SOURCE_WDT_MASK;
	writel(val, wdt->rst_source);

	writel(0, wdt->tmr_regs + TIMER_PTV);
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);
}

static int tegra20_wdt_start(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);
	int ret = 0;

	mutex_lock(&wdt->lock);

	if (wdt->vde_off) {
		dev_warn(wdd->parent,
			 "not starting, a reset would hang with VDE powered off\n");
		ret = -EBUSY;
	} else {
		__tegra20_wdt_start(wdt);
	}

	mutex_unlock(&wdt->lock);

	return ret;
}

static int tegra20_wdt_stop(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	mutex_lock(&wdt->lock);
	__tegra20_wdt_stop(wdt);
	mutex_unlock(&wdt->lock);

	return 0;
}

static int tegra20_wdt_ping(struct watchdog_device *wdd)
{
	struct tegra_wdt *wdt = watchdog_get_drvdata(wdd);

	mutex_lock(&wdt->lock);

	/*
	 * Clearing the interrupt alone doesn't restart the timer's period, so
	 * the reset could follow as soon as half the timeout after the ping.
	 * Rewriting the timer setting restarts the period. The interrupt is
	 * cleared afterwards, so one fired in between doesn't count. While
	 * stopped, the timer setting reads back as 0, so this doesn't start it.
	 */
	writel(readl(wdt->tmr_regs + TIMER_PTV), wdt->tmr_regs + TIMER_PTV);
	writel(TIMER_PCR_INTR_CLR, wdt->tmr_regs + TIMER_PCR);

	mutex_unlock(&wdt->lock);

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

static const struct watchdog_ops tegra20_wdt_ops = {
	.owner = THIS_MODULE,
	.start = tegra20_wdt_start,
	.stop = tegra20_wdt_stop,
	.ping = tegra20_wdt_ping,
	.set_timeout = tegra20_wdt_set_timeout,
};

static int tegra20_wdt_powergate_notify(struct notifier_block *nb,
					unsigned long event, void *data)
{
	struct tegra_wdt *wdt = container_of(nb, struct tegra_wdt,
					     powergate_nb);
	unsigned int id = *(unsigned int *)data;

	if (event != TEGRA_PMC_POWERGATE_POWER_OFF ||
	    id != TEGRA_POWERGATE_VDEC)
		return NOTIFY_DONE;

	mutex_lock(&wdt->lock);

	if (!wdt->vde_off) {
		wdt->vde_off = true;
		__tegra20_wdt_stop(wdt);

		/*
		 * Stop the core's pings. If user space has the device open,
		 * its pings don't restart the watchdog either.
		 */
		clear_bit(WDOG_HW_RUNNING, &wdt->wdd.status);

		dev_info(wdt->wdd.parent,
			 "VDE powered off, watchdog stopped\n");
	}

	mutex_unlock(&wdt->lock);

	return NOTIFY_OK;
}

static void tegra20_wdt_unregister_notifier(void *data)
{
	struct tegra_wdt *wdt = data;

	tegra_pmc_powergate_unregister_notifier(&wdt->powergate_nb);
}

static int tegra20_wdt_init(struct platform_device *pdev, struct tegra_wdt *wdt,
			    void __iomem *regs)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct resource res;
	int ret;

	/*
	 * RST_SOURCE belongs to the clock and reset controller, whose driver
	 * doesn't use it. Map only this register, without requesting it, as
	 * the region belongs to the clock and reset controller.
	 */
	np = of_find_compatible_node(NULL, NULL, "nvidia,tegra20-car");
	if (!np)
		return dev_err_probe(dev, -ENODEV,
				     "clock and reset controller not found\n");

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;

	wdt->rst_source = devm_ioremap(dev, res.start + T20_RST_SOURCE,
				       sizeof(u32));
	if (!wdt->rst_source)
		return -ENOMEM;

	wdt->tmr_regs = regs + T20_WDT_TIMER_BASE;

	ret = devm_mutex_init(dev, &wdt->lock);
	if (ret)
		return ret;

	/*
	 * If the VDE partition is already powered off, the notifier is called
	 * right away and stops a watchdog left running by the bootloader.
	 */
	if (stop_on_vde_off) {
		wdt->powergate_nb.notifier_call = tegra20_wdt_powergate_notify;

		ret = tegra_pmc_powergate_register_notifier(&wdt->powergate_nb);
		if (ret) {
			dev_warn(dev, "failed to register VDE notifier: %d\n",
				 ret);
		} else {
			ret = devm_add_action_or_reset(dev,
					tegra20_wdt_unregister_notifier, wdt);
			if (ret)
				return ret;
		}
	}

	/*
	 * The bootloader may leave the watchdog running. Take it over with
	 * our own timeout and timer 1, and let the core ping it until user
	 * space opens the device.
	 */
	mutex_lock(&wdt->lock);

	if (!wdt->vde_off &&
	    (readl(wdt->rst_source) & T20_RST_SOURCE_WDT_EN)) {
		dev_info(dev, "watchdog left running by the bootloader, taking it over (timeout %u sec)\n",
			 wdt->wdd.timeout);
		__tegra20_wdt_start(wdt);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	mutex_unlock(&wdt->lock);

	return 0;
}

static const struct tegra_wdt_soc tegra20_wdt_soc = {
	.ops = &tegra20_wdt_ops,
	.max_timeout = T20_MAX_WDT_TIMEOUT,
	.init = tegra20_wdt_init,
};

static const struct tegra_wdt_soc tegra30_wdt_soc = {
	.ops = &tegra_wdt_ops,
	.max_timeout = MAX_WDT_TIMEOUT,
	.init = tegra30_wdt_init,
};

static int tegra_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct tegra_wdt_soc *soc;
	struct watchdog_device *wdd;
	struct tegra_wdt *wdt;
	void __iomem *regs;
	int ret;

	soc = device_get_match_data(dev);

	/* This is the timer base. */
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	/*
	 * Allocate our watchdog driver data, which has the
	 * struct watchdog_device nested within it.
	 */
	wdt = devm_kzalloc_obj(dev, *wdt);
	if (!wdt)
		return -ENOMEM;

	/* Initialize struct watchdog_device. */
	wdd = &wdt->wdd;
	wdd->timeout = heartbeat;
	wdd->info = &tegra_wdt_info;
	wdd->ops = soc->ops;
	wdd->min_timeout = MIN_WDT_TIMEOUT;
	wdd->max_timeout = soc->max_timeout;
	wdd->parent = dev;

	watchdog_set_drvdata(wdd, wdt);

	watchdog_set_nowayout(wdd, nowayout);

	/* Initialize struct tegra_wdt. */
	ret = soc->init(pdev, wdt, regs);
	if (ret)
		return ret;

	watchdog_stop_on_unregister(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, wdt);

	dev_info(dev, "initialized (heartbeat = %d sec, nowayout = %d)\n",
		 heartbeat, nowayout);

	return 0;
}

/*
 * The watchdog may also run without user space having opened it, when it was
 * taken over from the bootloader, so check whether the hardware runs. Calling
 * the stop operation directly leaves WDOG_HW_RUNNING set for the resume.
 */
static int tegra_wdt_suspend(struct device *dev)
{
	struct tegra_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&wdt->wdd))
		wdt->wdd.ops->stop(&wdt->wdd);

	return 0;
}

static int tegra_wdt_resume(struct device *dev)
{
	struct tegra_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&wdt->wdd))
		wdt->wdd.ops->start(&wdt->wdd);

	return 0;
}

/*
 * Tegra30 and later list "nvidia,tegra20-timer" as well, but match their more
 * specific compatible string.
 */
static const struct of_device_id tegra_wdt_of_match[] = {
	{ .compatible = "nvidia,tegra30-timer", .data = &tegra30_wdt_soc },
	{ .compatible = "nvidia,tegra20-timer", .data = &tegra20_wdt_soc },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_wdt_of_match);

static DEFINE_SIMPLE_DEV_PM_OPS(tegra_wdt_pm_ops,
				tegra_wdt_suspend, tegra_wdt_resume);

static struct platform_driver tegra_wdt_driver = {
	.probe		= tegra_wdt_probe,
	.driver		= {
		.name	= "tegra-wdt",
		.pm	= pm_sleep_ptr(&tegra_wdt_pm_ops),
		.of_match_table = tegra_wdt_of_match,
	},
};
module_platform_driver(tegra_wdt_driver);

MODULE_AUTHOR("NVIDIA Corporation");
MODULE_DESCRIPTION("Tegra Watchdog Driver");
MODULE_LICENSE("GPL v2");
