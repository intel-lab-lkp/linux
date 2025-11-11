// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Realtek Semiconductor Corp.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/irqflags.h>
#include <linux/interrupt.h>
#include "timer-of.h"

#define ENBL 1
#define DSBL 0

#define SYSTIMER_RATE 1000000
#define SYSTIMER_MIN_DELTA 0x64
#define SYSTIMER_MAX_DELTA ULONG_MAX

/* SYSTIMER Register Offset (RTK Internal Use) */
#define TS_LW_OFST 0x0
#define TS_HW_OFST 0x4
#define TS_CMP_VAL_LW_OFST 0x8
#define TS_CMP_VAL_HW_OFST 0xC
#define TS_CMP_CTRL_OFST 0x10
#define TS_CMP_STAT_OFST 0x14

/* SYSTIMER CMP CTRL REG Mask */
#define TS_CMP_EN_MASK 0x1
#define TS_WR_EN0_MASK 0x2

static void __iomem *systimer_base;

static u64 rtk_ts64_read(void)
{
	u64 ts;
	u32 low, high;

	/* Caution: Read LSB word (TS_LW_OFST) first then MSB (TS_HW_OFST) */
	low = readl(systimer_base + TS_LW_OFST);
	high = readl(systimer_base + TS_HW_OFST);

	pr_debug("64bit-TS:HW=0x%08x,LW=0x%08x\n", high, low);
	ts = ((u64)high << 32) | low;

	return ts;
}

static void rtk_cmp_value_write(u64 value)
{
	u32 high, low;

	low = value & 0xFFFFFFFF;
	high = value >> 32;
	pr_debug("Write 64bit-CMP:HW=0x%08x,LW=0x%08x\n", high, low);

	writel(high, systimer_base + TS_CMP_VAL_HW_OFST);
	writel(low, systimer_base + TS_CMP_VAL_LW_OFST);
}

static inline void rtk_cmp_en_write(bool cmp_en)
{
	u32 val;

	val = TS_WR_EN0_MASK;
	if (cmp_en == ENBL)
		val |= TS_CMP_EN_MASK;

	writel(val, systimer_base + TS_CMP_CTRL_OFST);
	pr_debug("Write TS CMP CTRL = 0x%08x\n", val);
}

static int rtk_syst_clkevt_next_ktime(ktime_t expires,
				      struct clock_event_device *clkevt)
{
	u64 cmp_val;
	unsigned long flags;
	ktime_t now = ktime_get();
	s64 delta_ns = ktime_to_ns(ktime_sub(expires, now));
	u64 delta_us = delta_ns / 1000;

	pr_debug("delta_ns = %lld, clkevt.min_delta_ns = %llu\n",
		 delta_ns, clkevt->min_delta_ns);

	if (delta_ns <= (s64)clkevt->min_delta_ns) {
		delta_ns = clkevt->min_delta_ns;
		delta_us = delta_ns / 1000;
		pr_debug("Clamping delta_ns to min_delta_ns\n");
	}

	rtk_cmp_en_write(DSBL);
	local_irq_save(flags);
	cmp_val = rtk_ts64_read();

	/* Set CMP value to current timestamp plus delta_us */
	rtk_cmp_value_write(cmp_val + delta_us);
	rtk_cmp_en_write(ENBL);
	local_irq_restore(flags);
	return 0;
}

static irqreturn_t rtk_ts_match_intr_handler(int irq, void *dev_id)
{
	u32 val;
	void __iomem *reg_base;
	struct clock_event_device *clkevt = dev_id;

	/* Disable TS CMP Match */
	rtk_cmp_en_write(DSBL);

	/* Clear TS CMP INTR */
	reg_base = systimer_base + TS_CMP_STAT_OFST;
	val = readl(reg_base) & TS_CMP_EN_MASK;
	writel(val | TS_CMP_EN_MASK, reg_base);

	clkevt->event_handler(clkevt);

	return IRQ_HANDLED;
}

static int rtk_syst_shutdown(struct clock_event_device *clkevt)
{
	void __iomem *reg_base;
	u64 cmp_val = 0;

	/* Disable TS CMP Match */
	rtk_cmp_en_write(DSBL);
	/* Set compare value to 0 */
	rtk_cmp_value_write(cmp_val);

	/* Clear TS CMP INTR */
	reg_base = systimer_base + TS_CMP_STAT_OFST;
	writel(TS_CMP_EN_MASK, reg_base);
	return 0;
}

static struct timer_of _to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE,

	.clkevt = {
		.name = "rtk-clkevt",
		.rating = 300,
		.cpumask = cpu_possible_mask,
		.features = CLOCK_EVT_FEAT_DYNIRQ |
			    CLOCK_EVT_FEAT_ONESHOT |
			    CLOCK_EVT_FEAT_KTIME,
		.set_next_ktime = rtk_syst_clkevt_next_ktime,
		.set_state_oneshot = rtk_syst_shutdown,
		.set_state_shutdown = rtk_syst_shutdown
	},

	.of_irq = {
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
		.handler = rtk_ts_match_intr_handler
	},
};

static int __init rtk_systimer_init(struct device_node *node)
{
	int ret;

	ret = timer_of_init(node, &_to);
	if (ret)
		return ret;

	systimer_base = timer_of_base(&_to);
	clockevents_config_and_register(&_to.clkevt, SYSTIMER_RATE,
					SYSTIMER_MIN_DELTA, SYSTIMER_MAX_DELTA);

	pr_info("Realtek SYSTIMER registered\n");
	return 0;
}

TIMER_OF_DECLARE(rtk_systimer, "realtek,systimer", rtk_systimer_init);
