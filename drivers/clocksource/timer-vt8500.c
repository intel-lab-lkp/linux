// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  arch/arm/mach-vt8500/timer.c
 *
 *  Copyright (C) 2012 Tony Prisk <linux@prisktech.co.nz>
 *  Copyright (C) 2010 Alexey Charkov <alchark@gmail.com>
 */

/*
 * This file is copied and modified from the original timer.c provided by
 * Alexey Charkov. Minor changes have been made for Device Tree Support.
 */

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/delay.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <linux/watchdog.h>

#define VT8500_TIMER_OFFSET	0x0100
#define VT8500_TIMER_HZ		3000000

#define TIMER_MATCH_REG(x)	(4 * (x))
#define TIMER_COUNT_REG		0x0010	 /* clocksource counter */

#define TIMER_STATUS_REG	0x0014
#define TIMER_STATUS_MATCH(x)	BIT((x))
#define TIMER_STATUS_CLEARALL	(TIMER_STATUS_MATCH(0) | \
				 TIMER_STATUS_MATCH(1) | \
				 TIMER_STATUS_MATCH(2) | \
				 TIMER_STATUS_MATCH(3))

#define TIMER_WATCHDOG_EN_REG	0x0018
#define TIMER_WD_EN		BIT(0)

#define TIMER_INT_EN_REG	0x001c	 /* interrupt enable */
#define TIMER_INT_EN_MATCH(x)	BIT((x))

#define TIMER_CTRL_REG		0x0020
#define TIMER_CTRL_ENABLE	BIT(0)	 /* enable clocksource counter */
#define TIMER_CTRL_RD_REQ	BIT(1)	 /* request counter read */

#define TIMER_ACC_STS_REG	0x0024	 /* access status */
#define TIMER_ACC_WR_MATCH(x)	BIT((x)) /* writing Match (x) value */
#define TIMER_ACC_WR_COUNTER	BIT(4)	 /* writing clocksource counter */
#define TIMER_ACC_RD_COUNTER	BIT(5)	 /* reading clocksource counter */

#define msecs_to_loops(t) (loops_per_jiffy / 1000 * HZ * t)

#define MIN_OSCR_DELTA		16

static void __iomem *regbase;
static unsigned int sys_timer_ch;	 /* which match register to use
					  * for the system timer
					  */

static u64 vt8500_timer_read(struct clocksource *cs)
{
	int loops = msecs_to_loops(10);

	writel(TIMER_CTRL_ENABLE | TIMER_CTRL_RD_REQ, regbase + TIMER_CTRL_REG);
	while (readl(regbase + TIMER_ACC_STS_REG) & TIMER_ACC_RD_COUNTER
	     && --loops)
		cpu_relax();
	return readl(regbase + TIMER_COUNT_REG);
}

static struct clocksource clocksource = {
	.name           = "vt8500_timer",
	.rating         = 200,
	.read           = vt8500_timer_read,
	.mask           = CLOCKSOURCE_MASK(32),
	.flags          = CLOCK_SOURCE_IS_CONTINUOUS,
};

static int vt8500_timer_set_next_event(unsigned long cycles,
				    struct clock_event_device *evt)
{
	int loops = msecs_to_loops(10);
	u64 alarm = clocksource.read(&clocksource) + cycles;

	while (readl(regbase + TIMER_ACC_STS_REG) & TIMER_ACC_WR_MATCH(sys_timer_ch)
	       && --loops)
		cpu_relax();
	writel((unsigned long)alarm, regbase + TIMER_MATCH_REG(sys_timer_ch));

	if ((signed)(alarm - clocksource.read(&clocksource)) <= MIN_OSCR_DELTA)
		return -ETIME;

	writel(TIMER_INT_EN_MATCH(sys_timer_ch), regbase + TIMER_INT_EN_REG);

	return 0;
}

static int vt8500_shutdown(struct clock_event_device *evt)
{
	writel(readl(regbase + TIMER_CTRL_REG) | TIMER_CTRL_ENABLE,
	       regbase + TIMER_CTRL_REG);
	writel(0, regbase + TIMER_INT_EN_REG);
	return 0;
}

static struct clock_event_device clockevent = {
	.name			= "vt8500_timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 200,
	.set_next_event		= vt8500_timer_set_next_event,
	.set_state_shutdown	= vt8500_shutdown,
	.set_state_oneshot	= vt8500_shutdown,
};

static irqreturn_t vt8500_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	writel(TIMER_STATUS_CLEARALL, regbase + TIMER_STATUS_REG);
	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static int vt8500_watchdog_start(struct watchdog_device *wdd)
{
	u64 cycles = wdd->timeout * VT8500_TIMER_HZ;
	u64 deadline = clocksource.read(&clocksource) + cycles;

	writel((u32)deadline, regbase + TIMER_MATCH_REG(0));
	writel(TIMER_WD_EN, regbase + TIMER_WATCHDOG_EN_REG);
	return 0;
}

static int vt8500_watchdog_stop(struct watchdog_device *wdd)
{
	writel(0, regbase + TIMER_WATCHDOG_EN_REG);
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

static struct watchdog_device vt8500_watchdog_device = {
	.ops			= &vt8500_watchdog_ops,
	.info			= &vt8500_watchdog_info,
	.max_hw_heartbeat_ms	= -1UL / (VT8500_TIMER_HZ / 1000),
};

static int __init vt8500_timer_init(struct device_node *np)
{
	int timer_irq, ret;

	regbase = of_iomap(np, 0);
	if (!regbase) {
		pr_err("%s: Missing iobase description in Device Tree\n",
								__func__);
		return -ENXIO;
	}

	sys_timer_ch = of_irq_count(np) > 1 ? 1 : 0;

	timer_irq = irq_of_parse_and_map(np, sys_timer_ch);
	if (!timer_irq) {
		pr_err("%s: Missing irq description in Device Tree\n",
								__func__);
		return -EINVAL;
	}

	writel(TIMER_CTRL_ENABLE, regbase + TIMER_CTRL_REG);
	writel(TIMER_STATUS_CLEARALL, regbase + TIMER_STATUS_REG);
	writel(~0, regbase + TIMER_MATCH_REG(sys_timer_ch));

	ret = clocksource_register_hz(&clocksource, VT8500_TIMER_HZ);
	if (ret) {
		pr_err("%s: clocksource_register failed for %s\n",
		       __func__, clocksource.name);
		return ret;
	}

	clockevent.cpumask = cpumask_of(0);

	ret = request_irq(timer_irq, vt8500_timer_interrupt,
			  IRQF_TIMER | IRQF_IRQPOLL, "vt8500_timer",
			  &clockevent);
	if (ret) {
		pr_err("%s: setup_irq failed for %s\n", __func__,
							clockevent.name);
		return ret;
	}

	clockevents_config_and_register(&clockevent, VT8500_TIMER_HZ,
					MIN_OSCR_DELTA * 2, 0xf0000000);

	if (!IS_ENABLED(CONFIG_VT8500_TIMER_WATCHDOG))
		return 0;

	if (!sys_timer_ch) {
		pr_info("%s: Not enabling watchdog: only one irq was given\n",
			__func__);
	} else {
		watchdog_register_device(&vt8500_watchdog_device);
	}

	return 0;
}

TIMER_OF_DECLARE(vt8500, "via,vt8500-timer", vt8500_timer_init);
