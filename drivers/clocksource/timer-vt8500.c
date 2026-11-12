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

#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/delay.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include <clocksource/vt8500-timer.h>

#define TIMER_MATCH_REG(x)	(4 * (x))
#define TIMER_COUNT_REG		0x0010	 /* clocksource counter */

#define TIMER_STATUS_REG	0x0014
#define TIMER_STATUS_MATCH(x)	BIT((x))
#define TIMER_STATUS_CLEARALL	(TIMER_STATUS_MATCH(0) | \
				 TIMER_STATUS_MATCH(1) | \
				 TIMER_STATUS_MATCH(2) | \
				 TIMER_STATUS_MATCH(3))

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

/* Which match register to use for the system timer */
static unsigned int sys_timer_ch;

static u64 vt8500_timer_read(struct clocksource *cs)
{
	int loops = msecs_to_loops(10);

	writel(TIMER_CTRL_ENABLE | TIMER_CTRL_RD_REQ, regbase + TIMER_CTRL_REG);
	while (readl(regbase + TIMER_ACC_STS_REG) & TIMER_ACC_RD_COUNTER &&
	       --loops)
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

static u64 vt8500_timer_next(u64 cycles)
{
	return clocksource.read(&clocksource) + cycles;
}

static int vt8500_timer_set_next_event(unsigned long cycles,
				    struct clock_event_device *evt)
{
	int loops = msecs_to_loops(10);
	u64 alarm = vt8500_timer_next(cycles);

	while (readl(regbase + TIMER_ACC_STS_REG) &
	       TIMER_ACC_WR_MATCH(sys_timer_ch) && --loops)
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

static int __init vt8500_timer_init(struct device_node *np)
{
	int timer_irq, ret;

	regbase = of_iomap(np, 0);
	if (!regbase) {
		pr_err("%s: Missing iobase description in Device Tree\n",
								__func__);
		return -ENXIO;
	}

	/*
	 * The watchdog function is available on channel 0, but not on other
	 * channels. So to use it, the clock event device must use a different
	 * match register (1~3) and respective interrupt.
	 *
	 * Check if at least two interrupts are provided by the device tree,
	 * then use match register 1 for system clock events and reserve match
	 * register 0 for the watchdog.
	 */
	sys_timer_ch = of_irq_count(np) > 1 ? 1 : 0;

	timer_irq = irq_of_parse_and_map(np, sys_timer_ch);
	if (!timer_irq) {
		pr_err("%s: Missing irq description in Device Tree\n",
								__func__);
		iounmap(regbase);
		regbase = NULL;
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

	return 0;
}

static void vt8500_timer_aux_uninit(void *data)
{
	auxiliary_device_uninit(data);
}

static void vt8500_timer_aux_delete(void *data)
{
	auxiliary_device_delete(data);
}

static void vt8500_timer_aux_release(struct device *dev)
{
	struct vt8500_wdt_info *wdt_info;
	struct auxiliary_device *auxdev;

	auxdev = container_of(dev, struct auxiliary_device, dev);
	wdt_info = container_of(auxdev, struct vt8500_wdt_info, auxdev);
	kfree(wdt_info);
}

/*
 * This probe gets called after vt8500_timer_init(), which leaves regbase set
 * only once the clocksource counter is running: that counter and match
 * register 0 are all the watchdog needs, even if registering the clocksource
 * or the clock event device failed afterwards. Create the watchdog device as
 * a child, since the registers are shared.
 */
static int vt8500_timer_probe(struct platform_device *pdev)
{
	struct vt8500_wdt_info *wdt_info;
	struct device *dev = &pdev->dev;
	int ret;

	if (!regbase)
		return dev_err_probe(dev, -ENODEV,
				     "Timer not initialized, cannot create watchdog\n");

	if (!sys_timer_ch) {
		dev_info(dev, "Not enabling watchdog: only one irq was given\n");
		return 0;
	}

	wdt_info = kzalloc_obj(*wdt_info);
	if (!wdt_info)
		return -ENOMEM;

	wdt_info->timer_next = &vt8500_timer_next;
	wdt_info->regbase = regbase;
	wdt_info->auxdev.name = "vt8500-wdt";
	wdt_info->auxdev.dev.parent = dev;
	wdt_info->auxdev.dev.release = &vt8500_timer_aux_release;

	ret = auxiliary_device_init(&wdt_info->auxdev);
	if (ret) {
		kfree(wdt_info);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, vt8500_timer_aux_uninit,
				       &wdt_info->auxdev);
	if (ret)
		return ret;

	ret = auxiliary_device_add(&wdt_info->auxdev);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, vt8500_timer_aux_delete,
					&wdt_info->auxdev);
}

static const struct of_device_id vt8500_timer_of_match[] = {
	{ .compatible = "via,vt8500-timer", },
	{},
};

static struct platform_driver vt8500_timer_driver = {
	.probe  = vt8500_timer_probe,
	.driver = {
		.name = "vt8500-timer",
		.of_match_table = vt8500_timer_of_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(vt8500_timer_driver);

TIMER_OF_DECLARE(vt8500, "via,vt8500-timer", vt8500_timer_init);
