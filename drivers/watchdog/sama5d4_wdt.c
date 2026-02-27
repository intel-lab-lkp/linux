// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Atmel SAMA5D4 Watchdog Timer
 *
 * Copyright (C) 2015-2019 Microchip Technology Inc. and its subsidiaries
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/watchdog.h>

#include "at91sam9_wdt.h"

/* minimum and maximum watchdog timeout, in seconds */
#define MIN_WDT_TIMEOUT		1
#define MAX_WDT_TIMEOUT		16
#define WDT_DEFAULT_TIMEOUT	MAX_WDT_TIMEOUT

#define WDT_SEC2TICKS(s)	((s) ? (((s) << 8) - 1) : 0)

struct sama5d4_wdt {
	struct watchdog_device	wdd;
	void __iomem		*reg_base;
	u32			mr;
	u32			ir;
	u32			wddis_mask;
	unsigned long		last_ping;
	bool			need_irq;
	bool			is_modern_chip;
};

static inline bool wdt_enabled(struct sama5d4_wdt *wdt, u32 mr)
{
	return !(mr & wdt->wddis_mask);
}

static int wdt_timeout;
static bool nowayout = WATCHDOG_NOWAYOUT;

module_param(wdt_timeout, int, 0);
MODULE_PARM_DESC(wdt_timeout,
	"Watchdog timeout in seconds. (default = "
	__MODULE_STRING(WDT_DEFAULT_TIMEOUT) ")");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
	"Watchdog cannot be stopped once started (default="
	__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");


#define wdt_read(wdt, field) \
	readl_relaxed((wdt)->reg_base + (field))

/* 4 slow clock periods is 4/32768 = 122.07µs*/
#define WDT_DELAY	usecs_to_jiffies(123)

static bool check_is_modern_chip_by_compatible(struct device *dev)
{
	if (of_device_is_compatible(dev->of_node, "microchip,sam9x60-wdt") ||
	    of_device_is_compatible(dev->of_node, "microchip,sama7g5-wdt"))
		return true;

	return false;
}

static void wdt_write(struct sama5d4_wdt *wdt, u32 field, u32 val)
{
	/*
	 * WDT_CR and WDT_MR must not be modified within three slow clock
	 * periods following a restart of the watchdog performed by a write
	 * access in WDT_CR.
	 */
	while (time_before(jiffies, wdt->last_ping + WDT_DELAY))
		usleep_range(30, 125);
	writel_relaxed(val, wdt->reg_base + field);
	wdt->last_ping = jiffies;
}

static void wdt_write_nosleep(struct sama5d4_wdt *wdt, u32 field, u32 val)
{
	if (time_before(jiffies, wdt->last_ping + WDT_DELAY))
		udelay(123);
	writel_relaxed(val, wdt->reg_base + field);
	wdt->last_ping = jiffies;
}

static int wdt_start(struct watchdog_device *wdd)
{
	struct sama5d4_wdt *wdt = watchdog_get_drvdata(wdd);

	if (wdt->is_modern_chip)
		writel_relaxed(wdt->ir, wdt->reg_base + AT91_WDT_IER);
	wdt->mr &= ~wdt->wddis_mask;
	wdt_write(wdt, AT91_WDT_MR, wdt->mr);

	return 0;
}

static int wdt_stop(struct watchdog_device *wdd)
{
	struct sama5d4_wdt *wdt = watchdog_get_drvdata(wdd);

	if (wdt->is_modern_chip)
		writel_relaxed(wdt->ir, wdt->reg_base + AT91_WDT_IDR);
	wdt->mr |= wdt->wddis_mask;
	wdt_write(wdt, AT91_WDT_MR, wdt->mr);

	return 0;
}

static int wdt_ping(struct watchdog_device *wdd)
{
	struct sama5d4_wdt *wdt = watchdog_get_drvdata(wdd);

	wdt_write(wdt, AT91_WDT_CR, AT91_WDT_KEY | AT91_WDT_WDRSTT);

	return 0;
}

static int wdt_set_timeout(struct watchdog_device *wdd,
			   unsigned int timeout)
{
	struct sama5d4_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 value = WDT_SEC2TICKS(timeout);

	if (wdt->is_modern_chip) {
		wdt_write(wdt, AT91_WDT_WLR,
			  AT91_WDT_SET_COUNTER(value));

		wdd->timeout = timeout;
		return 0;
	}

	wdt->mr &= ~AT91_WDT_WDV;
	wdt->mr |= AT91_WDT_SET_WDV(value);

	/*
	 * WDDIS has to be 0 when updating WDD/WDV. The datasheet states: When
	 * setting the WDDIS bit, and while it is set, the fields WDV and WDD
	 * must not be modified.
	 * If the watchdog is enabled, then the timeout can be updated. Else,
	 * wait that the user enables it.
	 */
	if (wdt_enabled(wdt, wdt->mr))
		wdt_write(wdt, AT91_WDT_MR, wdt->mr & ~wdt->wddis_mask);

	wdd->timeout = timeout;

	return 0;
}

static const struct watchdog_info sama5d4_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.identity = "Atmel SAMA5D4 Watchdog",
};

static const struct watchdog_ops sama5d4_wdt_ops = {
	.owner = THIS_MODULE,
	.start = wdt_start,
	.stop = wdt_stop,
	.ping = wdt_ping,
	.set_timeout = wdt_set_timeout,
};

static irqreturn_t wdt_irq_handler(int irq, void *dev_id)
{
	struct sama5d4_wdt *wdt = platform_get_drvdata(dev_id);
	u32 reg;

	if (wdt->is_modern_chip)
		reg = wdt_read(wdt, AT91_WDT_ISR);
	else
		reg = wdt_read(wdt, AT91_WDT_SR);

	if (reg) {
		pr_crit("Atmel Watchdog Software Reset\n");
		emergency_restart();
		pr_crit("Reboot didn't succeed\n");
	}

	return IRQ_HANDLED;
}

static int of_wdt_init(struct device_node *np, struct sama5d4_wdt *wdt)
{
	const char *tmp;

	wdt->mr = wdt->wddis_mask;

	if (!of_property_read_string(np, "atmel,watchdog-type", &tmp) &&
	    !strcmp(tmp, "software"))
		wdt->need_irq = true;

	if (of_property_read_bool(np, "atmel,idle-halt"))
		wdt->mr |= AT91_WDT_WDIDLEHLT;

	if (of_property_read_bool(np, "atmel,dbg-halt"))
		wdt->mr |= AT91_WDT_WDDBGHLT;

	return 0;
}

static int wdt_init(struct sama5d4_wdt *wdt)
{
	struct watchdog_device *wdd = &wdt->wdd;
	u32 reg, val;

	val = WDT_SEC2TICKS(WDT_DEFAULT_TIMEOUT);

	/*
	 * When booting and resuming, the bootloader may have changed the
	 * watchdog configuration.
	 * If the watchdog is not running, we need to disable it properly.
	 * Otherwise, we can safely update it.
	 */
	reg = wdt_read(wdt, AT91_WDT_MR);
	if (wdt_enabled(wdt, reg)) {
		wdt->mr &= ~wdt->wddis_mask;
		set_bit(WDOG_HW_RUNNING, &wdd->status);
	} else {
		wdt->mr |= wdt->wddis_mask;
	}

	if (wdt->is_modern_chip) {
		if (wdt->need_irq)
			wdt->ir = AT91_WDT_PERINT;
		else
			wdt->mr |= AT91_WDT_PERIODRST;

		wdt_write(wdt, AT91_WDT_IER, wdt->ir);
		wdt_write(wdt, AT91_WDT_WLR, AT91_WDT_SET_COUNTER(val));
	} else {
		wdt->mr |= AT91_WDT_SET_WDD(WDT_SEC2TICKS(MAX_WDT_TIMEOUT));
		wdt->mr |= AT91_WDT_SET_WDV(val);

		if (wdt->need_irq)
			wdt->mr |= AT91_WDT_WDFIEN;
		else
			wdt->mr |= AT91_WDT_WDRSTEN;
	}

	wdt_write_nosleep(wdt, AT91_WDT_MR, wdt->mr);

	return 0;
}

static int wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	struct sama5d4_wdt *wdt;
	void __iomem *regs;
	u32 irq = 0;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdd = &wdt->wdd;
	wdd->timeout = WDT_DEFAULT_TIMEOUT;
	wdd->info = &sama5d4_wdt_info;
	wdd->ops = &sama5d4_wdt_ops;
	wdd->min_timeout = MIN_WDT_TIMEOUT;
	wdd->max_timeout = MAX_WDT_TIMEOUT;
	wdt->last_ping = jiffies;
	wdt->is_modern_chip = check_is_modern_chip_by_compatible(dev);
	if (wdt->is_modern_chip)
		wdt->wddis_mask = AT91_WDT_WDDIS_MODERN;
	else
		wdt->wddis_mask = AT91_WDT_WDDIS_LEGACY;

	watchdog_set_drvdata(wdd, wdt);

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	wdt->reg_base = regs;

	ret = of_wdt_init(dev->of_node, wdt);
	if (ret)
		return ret;

	if (wdt->need_irq) {
		irq = irq_of_parse_and_map(dev->of_node, 0);
		if (!irq) {
			dev_warn(dev, "failed to get IRQ from DT\n");
			wdt->need_irq = false;
		}
	}

	if (wdt->need_irq) {
		ret = devm_request_irq(dev, irq, wdt_irq_handler,
				       IRQF_SHARED | IRQF_IRQPOLL |
				       IRQF_NO_SUSPEND, pdev->name, pdev);
		if (ret) {
			dev_err(dev, "cannot register interrupt handler\n");
			return ret;
		}
	}

	watchdog_init_timeout(wdd, wdt_timeout, dev);

	ret = wdt_init(wdt);
	if (ret)
		return ret;

	watchdog_set_nowayout(wdd, nowayout);

	watchdog_stop_on_unregister(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, wdt);

	dev_info(dev, "initialized (timeout = %d sec, nowayout = %d)\n",
		 wdd->timeout, nowayout);

	return 0;
}

static const struct of_device_id wdt_of_match[] = {
	{
		.compatible = "atmel,sama5d4-wdt",
	},
	{
		.compatible = "microchip,sam9x60-wdt",
	},
	{
		.compatible = "microchip,sama7g5-wdt",
	},

	{ }
};
MODULE_DEVICE_TABLE(of, wdt_of_match);

static int wdt_suspend_late(struct device *dev)
{
	struct sama5d4_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		wdt_stop(&wdt->wdd);

	return 0;
}

static int wdt_resume_early(struct device *dev)
{
	struct sama5d4_wdt *wdt = dev_get_drvdata(dev);

	/*
	 * FIXME: writing MR also pings the watchdog which may not be desired.
	 * This should only be done when the registers are lost on suspend but
	 * there is no way to get this information right now.
	 */
	wdt_init(wdt);

	if (watchdog_active(&wdt->wdd))
		wdt_start(&wdt->wdd);

	return 0;
}

static const struct dev_pm_ops sama5d4_wdt_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(wdt_suspend_late,
				 wdt_resume_early)
};

static struct platform_driver sama5d4_wdt_driver = {
	.probe		= wdt_probe,
	.driver		= {
		.name	= "sama5d4_wdt",
		.pm	= pm_sleep_ptr(&sama5d4_wdt_pm_ops),
		.of_match_table = wdt_of_match,
	}
};
module_platform_driver(sama5d4_wdt_driver);

MODULE_AUTHOR("Atmel Corporation");
MODULE_DESCRIPTION("Atmel SAMA5D4 Watchdog Timer driver");
MODULE_LICENSE("GPL v2");
