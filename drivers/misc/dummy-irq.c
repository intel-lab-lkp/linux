// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dummy IRQ handler driver.
 *
 * This module only registers itself as a handler that is specified to it
 * by the 'irq' parameter.
 *
 * The sole purpose of this module is to help with debugging of systems on
 * which spurious IRQs would happen on disabled IRQ vector.
 *
 * Copyright (C) 2013 Jiri Kosina
 */

#include <linux/module.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

static int irq = -1;

static irqreturn_t dummy_interrupt(int irq, void *dev_id)
{
	static int count;

	if (count == 0) {
		pr_info("interrupt occurred on IRQ %d\n", irq);
		count++;
	}

	return IRQ_NONE;
}

static int __init dummy_irq_init(void)
{
	if (irq < 0) {
		pr_err("no IRQ given. Use irq=N\n");
		return -EIO;
	}
	if (request_irq(irq, &dummy_interrupt, IRQF_SHARED, "dummy_irq", &irq)) {
		pr_err("cannot register IRQ %d\n", irq);
		return -EIO;
	}
	pr_info("registered for IRQ %d\n", irq);
	return 0;
}

static void __exit dummy_irq_exit(void)
{
	pr_info("unloaded\n");
	free_irq(irq, &irq);
}

module_init(dummy_irq_init);
module_exit(dummy_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jiri Kosina");
module_param_hw(irq, uint, irq, 0444);
MODULE_PARM_DESC(irq, "The IRQ to register for");
MODULE_DESCRIPTION("Dummy IRQ handler driver");
