// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 *
 * Some code are borrow from
 * linux/drivers/clocksource/arm_arch_timer.c
 * Copyright (C) 2011 ARM Ltd.
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/watchdog.h>

#define CNTCR				0x00
#define CNTCV				0x08

#define CNTPCT				0x00
#define CNTP_CVAL			0x20
#define CNTP_CTL			0x2c
#define  CNTP_CTL_ENABLE		BIT(0)
#define  CNTP_CTL_IMASK			BIT(1)
#define  CNTP_CTL_ISTATUS		BIT(2)
#define CNTP_AIVAL_RELOAD		0x48
#define CNTP_AIVAL_CTL			0x4c
#define  CNTP_AIVAL_CTL_EN		BIT(0)
#define  CNTP_AIVAL_CTL_CLR		BIT(1)
#define CNTP_CFG			0x50
#define  CNTP_CFG_AIVAL_MASK		GENMASK(3, 0)
#define  CNTP_CFG_AIVAL_IMPL		1

#define WCS				0x00
#define  WCS_EN				BIT(0)
#define  WCS_WS0			BIT(1)
#define  WCS_WS1			BIT(2)
#define WOR				0x08
#define WCV				0x10

#define WRR				0x00

#define SSE_WDT_DEFAULT_SECONDS		30

enum sse_timer_mode {
	SSE_TIMER_NORMAL,
	SSE_TIMER_AUTOINC,
};

struct sse_timer_frame {
	void __iomem *base;
	struct clocksource cs;
	struct clock_event_device ce;
	u32 ticks_per_jiffy;
	int irq;
	enum sse_timer_mode mode;
};

struct sse_wdt_frame {
	struct watchdog_device wdd;
	void __iomem *ctrl_base;
	void __iomem *refr_base;
	unsigned long freq;
	int irq;
};

struct sse_timer {
	void __iomem *cntctrl_base;
	struct clk *clk;
	struct sse_timer_frame *timers;
	struct sse_wdt_frame *wdts;
	int timer_num;
	int wdt_num;
};

#define cs_to_sse_timer_frame(x) \
	container_of(x, struct sse_timer_frame, cs)
#define ce_to_sse_timer_frame(x) \
	container_of(x, struct sse_timer_frame, ce)

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static __always_inline u64 sse_get_cntpct(struct sse_timer_frame *f)
{
#ifndef CONFIG_64BIT
	u32 cnt_lo, cnt_hi, tmp_hi;

	do {
		cnt_hi = __le32_to_cpu((__le32 __force)__raw_readl(f->base + CNTPCT + 4));
		cnt_lo = __le32_to_cpu((__le32 __force)__raw_readl(f->base + CNTPCT));
		tmp_hi = __le32_to_cpu((__le32 __force)__raw_readl(f->base + CNTPCT + 4));
	} while (cnt_hi != tmp_hi);

	return ((u64) cnt_hi << 32) | cnt_lo;
#else
	return readq_relaxed(f->base + CNTPCT);
#endif

}

static int sse_ce_shutdown(struct clock_event_device *ce)
{
	struct sse_timer_frame *f = ce_to_sse_timer_frame(ce);
	u32 val;

	val = readl_relaxed(f->base + CNTP_CTL);
	val &= ~CNTP_CTL_ENABLE;
	writel_relaxed(val, f->base + CNTP_CTL);
	return 0;
}

static int sse_ce_set_oneshot(struct clock_event_device *ce)
{
	struct sse_timer_frame *f = ce_to_sse_timer_frame(ce);
	u32 val;

	f->mode = SSE_TIMER_NORMAL;

	val = readl_relaxed(f->base + CNTP_CTL);
	if (val & CNTP_CTL_ENABLE) {
		val &= ~CNTP_CTL_ENABLE;
		writel_relaxed(val, f->base + CNTP_CTL);
	}

	writel_relaxed(0, f->base + CNTP_AIVAL_CTL);

	return 0;
}

static int sse_ce_set_periodic(struct clock_event_device *ce)
{
	struct sse_timer_frame *f = ce_to_sse_timer_frame(ce);
	u32 val;

	f->mode = SSE_TIMER_AUTOINC;

	val = readl_relaxed(f->base + CNTP_CTL);
	if (val & CNTP_CTL_ENABLE) {
		val &= ~CNTP_CTL_ENABLE;
		writel_relaxed(val, f->base + CNTP_CTL);
	}

	val |= CNTP_CTL_ENABLE;
	val &= ~CNTP_CTL_IMASK;

	writel_relaxed(f->ticks_per_jiffy, f->base + CNTP_AIVAL_RELOAD);
	writel_relaxed(CNTP_AIVAL_CTL_EN, f->base + CNTP_AIVAL_CTL);
	writel_relaxed(val, f->base + CNTP_CTL);

	return 0;
}

static int sse_ce_next_event(unsigned long evt, struct clock_event_device *ce)
{
	struct sse_timer_frame *f = ce_to_sse_timer_frame(ce);
	u32 val;
	u64 cnt;

	val = readl_relaxed(f->base + CNTP_CTL);
	if (val & CNTP_CTL_ENABLE) {
		val &= ~CNTP_CTL_ENABLE;
		writel_relaxed(val, f->base + CNTP_CTL);
	}

	val |= CNTP_CTL_ENABLE;
	val &= ~CNTP_CTL_IMASK;

	cnt = sse_get_cntpct(f);
	writeq_relaxed(evt + cnt, f->base + CNTP_CVAL);
	writel_relaxed(val, f->base + CNTP_CTL);

	return 0;
}

static irqreturn_t sse_timer_interrupt(int irq, void *dev_id)
{
	struct sse_timer_frame *f = dev_id;
	u32 val;

	val = readl_relaxed(f->base + CNTP_CTL);
	if (!(val & CNTP_CTL_ISTATUS))
		return IRQ_NONE;

	if (f->mode == SSE_TIMER_AUTOINC) {
		val = readl_relaxed(f->base + CNTP_AIVAL_CTL);
		val &= ~CNTP_AIVAL_CTL_CLR;
		writel_relaxed(val, f->base + CNTP_AIVAL_CTL);
	} else {
		val |= CNTP_CTL_IMASK;
		writel_relaxed(val, f->base + CNTP_CTL);
	}
	f->ce.event_handler(&f->ce);

	return IRQ_HANDLED;
}

static int sse_setup_clockevent(struct device *dev, struct sse_timer_frame *f,
				unsigned long rate)
{
	int ret;
	u32 val = readl_relaxed(f->base + CNTP_CFG);

	f->ticks_per_jiffy = DIV_ROUND_UP(rate, HZ);

	f->ce.name = "sse-timer";
	f->ce.rating = 300;
	f->ce.irq = f->irq;
	f->ce.cpumask = cpu_possible_mask;
	f->ce.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_DYNIRQ;
	f->ce.set_state_shutdown = sse_ce_shutdown;
	f->ce.set_next_event = sse_ce_next_event;
	f->ce.set_state_oneshot_stopped = sse_ce_shutdown;
	if (FIELD_GET(CNTP_CFG_AIVAL_MASK, val) == CNTP_CFG_AIVAL_IMPL) {
		f->ce.set_state_periodic = sse_ce_set_periodic;
		f->ce.set_state_oneshot = sse_ce_set_oneshot;
	}

	clockevents_config_and_register(&f->ce, rate, 0xf, ULONG_MAX);

	ret = devm_request_irq(dev, f->irq, sse_timer_interrupt,
			       IRQF_TIMER | IRQF_NOBALANCING,
			       f->ce.name, f);
	if (ret) {
		dev_err(dev, "Unable to register interrupt\n");
		return ret;
	}

	return 0;
}

static noinstr u64 cs_sse_get_cntpct(struct clocksource *cs)
{
	struct sse_timer_frame *f = cs_to_sse_timer_frame(cs);

	return sse_get_cntpct(f);
}

static int sse_setup_clocksource(struct device *dev, struct sse_timer_frame *f,
				 unsigned long rate)
{
	int ret;

	f->cs.name = "sse-timer";
	f->cs.rating = 300;
	f->cs.read = cs_sse_get_cntpct;
	f->cs.mask = CLOCKSOURCE_MASK(64);
	f->cs.flags = CLOCK_SOURCE_IS_CONTINUOUS;

	ret = clocksource_register_hz(&f->cs, rate);
	if (ret) {
		dev_err(dev, "Couldn't register clock source.\n");
		return ret;
	}

	return 0;
}

static irqreturn_t sse_wdt_interrupt(int irq, void *dev_id)
{
	struct sse_wdt_frame *f = dev_id;
	u32 val;

	val = readl_relaxed(f->ctrl_base + WCS);
	if (val & WCS_WS0)
		watchdog_notify_pretimeout(&f->wdd);

	return IRQ_HANDLED;
}

static int sse_wdt_set_timeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct sse_wdt_frame *f = watchdog_get_drvdata(wdd);

	wdd->timeout = timeout;

	/* Pretimeout is 1/2 of timeout */
	wdd->pretimeout = timeout / 2;

	writel_relaxed(((u64)f->freq / 2) * timeout, f->ctrl_base + WOR);

	return 0;
}

static int sse_wdt_set_pretimeout(struct watchdog_device *wdd, unsigned int timeout)
{
	struct sse_wdt_frame *f = watchdog_get_drvdata(wdd);

	if (!timeout)
		return -EINVAL;

	/* pretimeout should not exceed 1/2 of max_timeout */
	if (timeout * 2 <= f->wdd.max_timeout)
		return sse_wdt_set_timeout(wdd, timeout * 2);

	return -EINVAL;
}

static int sse_wdt_ping(struct watchdog_device *wdd)
{
	struct sse_wdt_frame *f = watchdog_get_drvdata(wdd);

	writel_relaxed(0, f->refr_base + WRR);

	return 0;
}

static int sse_wdt_start(struct watchdog_device *wdd)
{
	struct sse_wdt_frame *f = watchdog_get_drvdata(wdd);

	writel_relaxed(WCS_EN, f->ctrl_base + WCS);

	return 0;
}

static int sse_wdt_stop(struct watchdog_device *wdd)
{
	struct sse_wdt_frame *f = watchdog_get_drvdata(wdd);

	writel_relaxed(0, f->ctrl_base + WCS);

	return 0;
}

static const struct watchdog_info sse_wdt_info = {
	.identity	= "ARM SSE Watchdog",
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_PRETIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE |
			  WDIOF_CARDRESET,
};

static const struct watchdog_ops sse_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= sse_wdt_start,
	.stop		= sse_wdt_stop,
	.ping		= sse_wdt_ping,
	.set_timeout	= sse_wdt_set_timeout,
	.set_pretimeout	= sse_wdt_set_pretimeout,
};

static int sse_setup_wdt(struct device *dev, struct sse_wdt_frame *f, unsigned long rate)
{
	u32 val;
	int ret;
	unsigned int max_pretimeout, timeout = SSE_WDT_DEFAULT_SECONDS;

	f->freq = rate;
	f->wdd.info = &sse_wdt_info;
	f->wdd.ops = &sse_wdt_ops;
	f->wdd.parent = dev;

	max_pretimeout = U32_MAX / f->freq;
	/* Maximum timeout is twice the pretimeout */
	f->wdd.max_timeout = max_pretimeout * 2;
	f->wdd.max_hw_heartbeat_ms = max_pretimeout * 1000;
	/* Minimum first timeout (pretimeout) is 1, so min_timeout as 2 */
	f->wdd.min_timeout = 2;

	val = readl_relaxed(f->ctrl_base + WCS);
	if (val & WCS_WS1) {
		dev_warn(dev, "System reset by WDT.\n");
		f->wdd.bootstatus |= WDIOF_CARDRESET;
	}

	if (val & WCS_EN) {
		timeout = readl_relaxed(f->ctrl_base + WOR) / 2 / f->freq;
		set_bit(WDOG_HW_RUNNING, &f->wdd.status);
	}

	watchdog_set_drvdata(&f->wdd, f);
	watchdog_set_nowayout(&f->wdd, nowayout);
	watchdog_init_timeout(&f->wdd, timeout, dev);
	watchdog_stop_on_reboot(&f->wdd);
	watchdog_stop_on_unregister(&f->wdd);

	ret = devm_watchdog_register_device(dev, &f->wdd);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, f->irq, sse_wdt_interrupt, 0, "sse-wdt", f);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register interrupt handler\n");

	return 0;
}

static int sse_timer_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sse_timer *sse;
	unsigned long rate;
	int i, j, ret;

	sse = devm_kzalloc(&pdev->dev, sizeof(*sse), GFP_KERNEL);
	if (!sse)
		return -ENOMEM;

	platform_set_drvdata(pdev, sse);

	sse->cntctrl_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sse->cntctrl_base)) {
		dev_err(&pdev->dev, "Can't map registers\n");
		return PTR_ERR(sse->cntctrl_base);
	}

	sse->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(sse->clk)) {
		dev_err(&pdev->dev, "Can't get timer clock\n");
		return PTR_ERR(sse->clk);
	}

	rate = clk_get_rate(sse->clk);
	if (!rate) {
		dev_err(&pdev->dev, "Couldn't get parent clock rate\n");
		return -EINVAL;
	}

	for_each_available_child_of_node_scoped(np, frame_node) {
		const __be32 *addr = of_get_address(frame_node, 1, NULL, NULL);

		if (!addr)
			sse->timer_num++;
		else
			sse->wdt_num++;
	}

	sse->timers = devm_kcalloc(&pdev->dev, sse->timer_num, sizeof(*sse->timers), GFP_KERNEL);
	if (!sse->timers)
		return -ENOMEM;
	sse->wdts = devm_kcalloc(&pdev->dev, sse->wdt_num, sizeof(*sse->wdts), GFP_KERNEL);
	if (!sse->wdts)
		return -ENOMEM;

	writel_relaxed(0, sse->cntctrl_base + CNTCR);
	writeq_relaxed(0, sse->cntctrl_base + CNTCV);
	writel_relaxed(1, sse->cntctrl_base + CNTCR);

	i = j = 0;
	for_each_available_child_of_node_scoped(np, frame_node) {
		struct resource res;
		void __iomem *base;
		const __be32 *addr;

		ret = of_address_to_resource(frame_node, 0, &res);
		if (ret < 0)
			return ret;
		base = devm_ioremap_resource(&pdev->dev, &res);
		if (IS_ERR(base))
			return PTR_ERR(base);

		addr = of_get_address(frame_node, 1, NULL, NULL);
		if (!addr) {
			sse->timers[i].base = base;

			ret = of_irq_get(frame_node, 0);
			if (ret < 0)
				return ret;
			sse->timers[i].irq = ret;
			i++;
		} else {
			sse->wdts[j].ctrl_base = base;

			ret = of_address_to_resource(frame_node, 1, &res);
			if (ret < 0)
				return ret;
			sse->wdts[j].refr_base = devm_ioremap_resource(&pdev->dev, &res);
			if (IS_ERR(sse->wdts[j].refr_base))
				return PTR_ERR(sse->wdts[j].refr_base);

			ret = of_irq_get(frame_node, 0);
			if (ret < 0)
				return ret;
			sse->wdts[j].irq = ret;
			j++;
		}
	}

	if (IS_ENABLED(CONFIG_WATCHDOG)) {
		for (i = 0; i < sse->wdt_num; i++) {
			ret = sse_setup_wdt(&pdev->dev, &sse->wdts[i], rate);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < sse->timer_num; i++) {
		ret = sse_setup_clocksource(&pdev->dev, &sse->timers[i], rate);
		if (ret)
			goto err_unreg_clocksource;
	}

	if (sse->timer_num > 0) {
		ret = sse_setup_clockevent(&pdev->dev, &sse->timers[0], rate);
		if (ret)
			goto err_unreg_clocksource;
	}

	return 0;

err_unreg_clocksource:
	for (j = 0; j < i; j++)
		clocksource_unregister(&sse->timers[j].cs);
	return ret;
}

static const struct of_device_id sse_timer_of_match[] = {
	{ .compatible = "arm,sse-timer" },
	{},
};
MODULE_DEVICE_TABLE(of, sse_timer_of_match);

static struct platform_driver sse_timer_driver = {
	.probe		= sse_timer_probe,
	.driver	= {
		.name	= "sse-timer",
		.of_match_table = sse_timer_of_match,
	},
};
module_platform_driver(sse_timer_driver);

MODULE_AUTHOR("Jisheng Zhang <jszhang@kernel.org>");
MODULE_DESCRIPTION("ARM SSE system timer driver");
MODULE_LICENSE("GPL");
