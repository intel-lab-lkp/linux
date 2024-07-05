// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 * Copyright (C) 2001 Ralf Baechle
 * Copyright (C) 2005  MIPS Technologies, Inc.	All rights reserved.
 *	Author: Maciej W. Rozycki <macro@mips.com>
 *
 * This file define the irq handler for MIPS CPU interrupts.
 */

/*
 * Almost all MIPS CPUs define 8 interrupt sources.  They are typically
 * level triggered (i.e., cannot be cleared from CPU; must be cleared from
 * device).
 *
 * The first two are software interrupts (i.e. not exposed as pins) which
 * may be used for IPIs in multi-threaded single-core systems.
 *
 * The last one is usually the CPU timer interrupt if the counter register
 * is present, or for old CPUs with an external FPU by convention it's the
 * FPU exception interrupt.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>

#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <asm/mipsmtregs.h>
#include <asm/setup.h>

static struct irq_domain *irq_domain __read_mostly;

static inline void unmask_mips_irq(struct irq_data *d)
{
	set_c0_status(IE_SW0 << d->hwirq);
	irq_enable_hazard();
}

static inline void mask_mips_irq(struct irq_data *d)
{
	clear_c0_status(IE_SW0 << d->hwirq);
	irq_disable_hazard();
}

static unsigned int mips_sw_irq_startup(struct irq_data *d)
{
	clear_c0_cause(C_SW0 << d->hwirq);
	back_to_back_c0_hazard();
	unmask_mips_irq(d);
	return 0;
}

static void mips_sw_irq_ack(struct irq_data *d)
{
	clear_c0_cause(C_SW0 << d->hwirq);
	back_to_back_c0_hazard();
}

static const struct irq_chip mips_cpu_irq_controller = {
	.name		= "MIPS",
	.irq_ack	= mask_mips_irq,
	.irq_mask	= mask_mips_irq,
	.irq_mask_ack	= mask_mips_irq,
	.irq_unmask	= unmask_mips_irq,
	.irq_eoi	= unmask_mips_irq,
	.irq_disable	= mask_mips_irq,
	.irq_enable	= unmask_mips_irq,
};

static const struct irq_chip mips_cpu_sw_irq_controller = {
	.name		= "MIPS",
	.irq_startup	= mips_sw_irq_startup,
	.irq_ack	= mips_sw_irq_ack,
	.irq_mask	= mask_mips_irq,
	.irq_unmask	= unmask_mips_irq,
};

#ifdef CONFIG_MIPS_MT
/*
 * Basically the same as above but taking care of all the MT stuff
 */
static unsigned int mips_mt_sw_irq_startup(struct irq_data *d)
{
	unsigned int vpflags = dvpe();

	clear_c0_cause(C_SW0 << d->hwirq);
	evpe(vpflags);
	unmask_mips_irq(d);
	return 0;
}

/*
 * While we ack the interrupt interrupts are disabled and thus we don't need
 * to deal with concurrency issues.
 */
static void mips_mt_sw_irq_ack(struct irq_data *d)
{
	unsigned int vpflags = dvpe();

	clear_c0_cause(C_SW0 << d->hwirq);
	evpe(vpflags);
}

static const struct irq_chip mips_mt_cpu_irq_controller = {
	.name		= "MIPS",
	.irq_startup	= mips_mt_sw_irq_startup,
	.irq_ack	= mips_mt_sw_irq_ack,
	.irq_mask	= mask_mips_irq,
	.irq_unmask	= unmask_mips_irq,
};
#endif

asmlinkage void __weak plat_irq_dispatch(void)
{
	unsigned long pending = read_c0_cause() & read_c0_status() & ST0_IM;
	int irq;

	if (!pending) {
		spurious_interrupt();
		return;
	}

	pending >>= CAUSEB_IP;
	while (pending) {
		irq = fls(pending) - 1;
		do_domain_IRQ(irq_domain, irq);
		pending &= ~BIT(irq);
	}
}

static int mips_cpu_intc_map(struct irq_domain *d, unsigned int irq,
			     irq_hw_number_t hw)
{
	const struct irq_chip *chip;

	if (hw < 2) {
		chip = &mips_cpu_sw_irq_controller;
#ifdef CONFIG_MIPS_MT
		if (cpu_has_mipsmt)
			chip = &mips_mt_cpu_irq_controller;
#endif
	} else {
		chip = &mips_cpu_irq_controller;
	}

	if (cpu_has_vint)
		set_vi_handler(hw, plat_irq_dispatch);

	irq_set_chip_and_handler(irq, chip, handle_percpu_irq);

	return 0;
}

static const struct irq_domain_ops mips_cpu_intc_irq_domain_ops = {
	.map = mips_cpu_intc_map,
	.xlate = irq_domain_xlate_onecell,
};

int mips_cpu_get_sw_int(int hwint)
{
	/* Only 0 and 1 for SW INT */
	WARN_ON(hwint > 1);

	if (!irq_domain)
		return 0;

	return irq_create_mapping(irq_domain, hwint);
}

static void __init __mips_cpu_irq_init(struct device_node *of_node)
{
	/* Mask interrupts. */
	clear_c0_status(ST0_IM);
	clear_c0_cause(CAUSEF_IP);

	irq_domain = irq_domain_add_legacy(of_node, 8, MIPS_CPU_IRQ_BASE, 0,
					   &mips_cpu_intc_irq_domain_ops,
					   NULL);
	if (!irq_domain)
		panic("Failed to add irqdomain for MIPS CPU");
}

void __init mips_cpu_irq_init(void)
{
	__mips_cpu_irq_init(NULL);
}

int __init mips_cpu_irq_of_init(struct device_node *of_node,
				struct device_node *parent)
{
	__mips_cpu_irq_init(of_node);
	return 0;
}
IRQCHIP_DECLARE(cpu_intc, "mti,cpu-interrupt-controller", mips_cpu_irq_of_init);
