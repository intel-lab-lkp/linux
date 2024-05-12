// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2024, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *  Loongson-3 Node Counter clocksource
 */

#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/sched_clock.h>

#include <loongson.h>
#include <loongson_regs.h>

#define NODECNT_REGBASE		0x3ff00408

static void __iomem *nodecnt_reg;
static u64 (*nodecnt_read_fn)(void);

static u64 notrace nodecnt_read_2x32(void)
{
	unsigned int hi, hi2, lo;

	do {
		hi = readl_relaxed(nodecnt_reg + 4);
		lo = readl_relaxed(nodecnt_reg);
		hi2 = readl_relaxed(nodecnt_reg + 4);
	} while (hi2 != hi);

	return (((u64) hi) << 32) + lo;
}

static u64 notrace nodecnt_read_64(void)
{
	return readq_relaxed(nodecnt_reg);
}

static u64 notrace nodecnt_read_csr(void)
{
	return csr_readq(LOONGSON_CSR_NODECNT);
}

static u64 nodecnt_clocksource_read(struct clocksource *cs)
{
	return nodecnt_read_fn();
}

static struct clocksource nodecnt_clocksource = {
	.name	= "nodecnt",
	.read	= nodecnt_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

int __init nodecnt_clocksource_init(void)
{
	int err;
	uint64_t delta;

	if (!cpu_clock_freq)
		return -ENODEV;

	if (cpu_has_csr() && csr_readl(LOONGSON_CSR_FEATURES) & LOONGSON_CSRF_NODECNT) {
		nodecnt_read_fn = nodecnt_read_csr;
	} else if (loongson_sysconf.bridgetype == VIRTUAL) {
		nodecnt_reg = ioremap(NODECNT_REGBASE, 8);
		if (!nodecnt_reg)
			return -ENOMEM;
		nodecnt_read_fn = nodecnt_read_64;
	} else {
		switch (boot_cpu_data.processor_id & (PRID_IMP_MASK | PRID_REV_MASK)) {
		case PRID_IMP_LOONGSON_64C | PRID_REV_LOONGSON3A_R2_0:
		case PRID_IMP_LOONGSON_64C | PRID_REV_LOONGSON3A_R2_1:
		case PRID_IMP_LOONGSON_64C | PRID_REV_LOONGSON3A_R3_0:
		case PRID_IMP_LOONGSON_64C | PRID_REV_LOONGSON3A_R3_1:
			break;
		default:
			return -ENODEV;
		}
		nodecnt_reg = ioremap(NODECNT_REGBASE, 8);
		if (!nodecnt_reg)
			return -ENOMEM;
		nodecnt_read_fn = nodecnt_read_2x32;
	}

	/* Test if nodecnt is usable */
	delta = nodecnt_read_fn();
	udelay(10);
	delta = nodecnt_read_fn() - delta;

	if (!delta) {
		pr_info("nodecnt: clocksource unusable\n");
		err = -ENODEV;
		goto out;
	}

	err = clocksource_register_hz(&nodecnt_clocksource, cpu_clock_freq);
	if (err) {
		pr_err("nodecnt: clocksource register failed\n");
		goto out;
	}

	/* It fits for sched_clock if we don't suffer from cross node access */
	if (loongson_sysconf.bridgetype == VIRTUAL || loongson_sysconf.nr_nodes <= 1)
		sched_clock_register(nodecnt_read_fn, 64, cpu_clock_freq);

out:
	if (nodecnt_reg)
		iounmap(nodecnt_reg);
	return err;
}
