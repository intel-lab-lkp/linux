// SPDX-License-Identifier: GPL-2.0
/*
 * ip30-power.c: Software powerdown and reset handling for IP30 architecture.
 *
 * Copyright (C) 2004-2007 Stanislaw Skowronek <skylark@unaligned.org>
 *               2014 Joshua Kinard <linux@kumba.dev>
 *               2009 Johannes Dickgreber <tanzy@gmx.de>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/notifier.h>
#include <linux/delay.h>
#include <linux/rtc/ds1685.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>

#include <asm/reboot.h>
#include <asm/sgi/heart.h>

static void __noreturn ip30_machine_restart(char *cmd)
{
	/*
	 * Execute HEART cold reset
	 *   Yes, it's cold-HEARTed!
	 */
	heart_write((heart_read(&heart_regs->mode) | HM_COLD_RST),
		    &heart_regs->mode);
	unreachable();
}

static void ip30_machine_power_off(void)
{
	struct device *dev;
	int cpu;

	if (!IS_REACHABLE(CONFIG_RTC_DRV_DS1685_FAMILY))
		return;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, "rtc-ds1685");
	if (!dev || !dev_get_drvdata(dev))
		return;

	/* Devices losing power may still interrupt; keep their handlers out. */
	for (cpu = 0; cpu < HEART_MAX_CPUS; cpu++)
		heart_write(HEART_CLR_ALL_MASK, &heart_regs->imr[cpu]);
	heart_write(HEART_ACK_ALL_MASK, &heart_regs->clear_isr);

	ds1685_rtc_poweroff(to_platform_device(dev));
}

#define IP30_POWERDOWN_TIMEOUT	120	/* seconds */

static bool ip30_shutting_down;

static void ip30_power_timeout(struct work_struct *work)
{
	ip30_machine_power_off();
}

static DECLARE_DELAYED_WORK(ip30_power_work, ip30_power_timeout);

/* Called by the RTC driver when the power button is pressed. */
void ip30_prepare_poweroff(void)
{
	/* A second press powers off at once. */
	if (ip30_shutting_down)
		ip30_machine_power_off();

	/* Shut down through userspace, powering off anyway if it stalls. */
	ip30_shutting_down = true;
	orderly_poweroff(true);
	schedule_delayed_work(&ip30_power_work, IP30_POWERDOWN_TIMEOUT * HZ);
}

static int __init ip30_reboot_setup(void)
{
	_machine_restart = ip30_machine_restart;

	/*
	 * The DS1687 RTC switches the power supply off.  Halt powers off
	 * too, as the power button needs a running kernel.
	 */
	if (IS_REACHABLE(CONFIG_RTC_DRV_DS1685_FAMILY)) {
		_machine_halt = ip30_machine_power_off;
		pm_power_off = ip30_machine_power_off;
	}

	return 0;
}

subsys_initcall(ip30_reboot_setup);
