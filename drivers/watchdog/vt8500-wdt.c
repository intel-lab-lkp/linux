// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: 2026 Alexey Charkov <alchark@gmail.com>

#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/container_of.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/moduleparam.h>
#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/watchdog.h>

#include <clocksource/vt8500-timer.h>

/* Offsets within the timer register block */
#define VT8500_WDT_MATCH_REG	0x0000	/* timer match register 0 */
#define VT8500_WDT_EN_REG	0x0018
#define VT8500_WDT_EN		BIT(0)
#define VT8500_WDT_ACC_STS_REG	0x0024	/* timer access status */
#define VT8500_WDT_ACC_WR_MATCH	BIT(0)	/* match register 0 write pending */

#define VT8500_WDT_DEFAULT_TIMEOUT	30	/* seconds */

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __stringify(WATCHDOG_NOWAYOUT) ")");

/* Arm the watchdog to reset the SoC @cycles counter ticks from now */
static int vt8500_watchdog_arm(struct vt8500_wdt_info *info, u64 cycles)
{
	u32 deadline, sts;
	int ret;

	/* Let a previous match write settle, as the timer driver does */
	ret = readl_poll_timeout_atomic(info->regbase + VT8500_WDT_ACC_STS_REG,
					sts, !(sts & VT8500_WDT_ACC_WR_MATCH),
					1, 10000);
	if (ret)
		return ret;

	/*
	 * The deadline is matched against the hardware clocksource counter,
	 * which is a u32 value incrementing at VT8500_TIMER_HZ and continuing
	 * past wraparound. When the return value of timer_next is greater than
	 * U32_MAX then the match should occur after the hardware counter wraps
	 * around, thus we take only the lower 32 bits of timer_next return val
	 */
	deadline = info->timer_next(cycles);
	writel(deadline, info->regbase + VT8500_WDT_MATCH_REG);
	writel(VT8500_WDT_EN, info->regbase + VT8500_WDT_EN_REG);

	return 0;
}

static int vt8500_watchdog_start(struct watchdog_device *wdd)
{
	struct vt8500_wdt_info *info = watchdog_get_drvdata(wdd);
	u32 timeout_ms = min(wdd->timeout * 1000, wdd->max_hw_heartbeat_ms);

	return vt8500_watchdog_arm(info,
				   (u64)timeout_ms * (VT8500_TIMER_HZ / 1000));
}

static int vt8500_watchdog_stop(struct watchdog_device *wdd)
{
	struct vt8500_wdt_info *info = watchdog_get_drvdata(wdd);

	writel(0, info->regbase + VT8500_WDT_EN_REG);

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

	wdd = devm_kzalloc_obj(&auxdev->dev, *wdd);
	if (!wdd)
		return -ENOMEM;

	wdd->info = &vt8500_watchdog_info;
	wdd->ops = &vt8500_watchdog_ops;
	wdd->min_timeout = 1;
	wdd->timeout = VT8500_WDT_DEFAULT_TIMEOUT;
	wdd->max_hw_heartbeat_ms = U32_MAX / (VT8500_TIMER_HZ / 1000);
	wdd->parent = &auxdev->dev;

	info = container_of(auxdev, struct vt8500_wdt_info, auxdev);
	watchdog_set_drvdata(wdd, info);
	watchdog_set_nowayout(wdd, nowayout);

	/* Let the core feed a watchdog left running, e.g. across kexec */
	if (readl(info->regbase + VT8500_WDT_EN_REG) & VT8500_WDT_EN)
		set_bit(WDOG_HW_RUNNING, &wdd->status);

	return devm_watchdog_register_device(&auxdev->dev, wdd);
}

static const struct auxiliary_device_id vt8500_wdt_ids[] = {
	{ .name = "timer_vt8500.vt8500-wdt" },
	{}
};

static struct auxiliary_driver vt8500_wdt_driver = {
	.name =	"vt8500-wdt",
	.probe = vt8500_wdt_probe,
	.id_table = vt8500_wdt_ids,
	.driver = {
		/* Unbinding would leave the hardware armed with no one to ping */
		.suppress_bind_attrs = true,
	},
};
builtin_driver(vt8500_wdt_driver, auxiliary_driver_register);
