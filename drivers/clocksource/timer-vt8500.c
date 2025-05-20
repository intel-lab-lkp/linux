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

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/vt8500-timer.h>

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

static u64 vt8500_timer_next(u64 cycles)
{
	return clocksource.read(&clocksource) + cycles;
}

static int vt8500_timer_set_next_event(unsigned long cycles,
				    struct clock_event_device *evt)
{
	int loops = msecs_to_loops(10);
	u64 alarm = vt8500_timer_next(cycles);

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
	struct auxiliary_device *aux;

	aux = container_of(dev, struct auxiliary_device, dev);
	kfree(aux);
}

/*
 * This probe gets called after the timer is already up and running. This will
 * create the watchdog device as a child since the registers are shared.
 */
static int vt8500_timer_probe(struct platform_device *pdev)
{
	struct vt8500_wdt_info *wdt_info;
	struct device *dev = &pdev->dev;
	int ret;

	if (!sys_timer_ch) {
		dev_info(dev, "Not enabling watchdog: only one irq was given");
		return 0;
	}

	if (!regbase)
		return dev_err_probe(dev, -ENOMEM,
			"Timer not initialized, cannot create watchdog");

	wdt_info = kzalloc(sizeof(*wdt_info), GFP_KERNEL);
	if (!wdt_info)
		return dev_err_probe(dev, -ENOMEM,
			"Failed to allocate vt8500-wdt info");

	wdt_info->timer_next = &vt8500_timer_next;
	wdt_info->wdt_en = regbase + TIMER_WATCHDOG_EN_REG;
	wdt_info->wdt_match = regbase + TIMER_MATCH_REG(0);
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
