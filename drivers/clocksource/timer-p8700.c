// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 MIPS.
 */

#include <linux/sched_clock.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/clocksource.h>

#include "timer-of.h"

static struct timer_of gcru_of = { .flags = TIMER_OF_BASE };
static u64 __iomem *p8700_time_val __ro_after_init;

static u64 notrace p8700_timer_sched_read(void)
{
	return (u64)readq_relaxed(p8700_time_val);
}

static int __init p8700_timer_init(struct device_node *node)
{
	int error = 0;

	error = timer_of_init(node, &gcru_of);
	if (error)
		return error;

	p8700_time_val = timer_of_base(&gcru_of);
	/* Now init the mmio timer with the address we got from DT */
	error = clocksource_mmio_init(p8700_time_val, "mips,p8700-gcru",
				      riscv_timebase, 450, 64,
				      clocksource_mmio_readq_up);
	if (error)
		return error;

	/* Sched clock */
	sched_clock_register(p8700_timer_sched_read, 64, riscv_timebase);

	return error;
}

TIMER_OF_DECLARE(p8700_timer, "mips,p8700-gcru", p8700_timer_init);
