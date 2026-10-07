// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 core driver providing the IRQ domain and IDA pools
 * shared by all transport drivers.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/spinlock.h>

static void nct6694_irq_enable(struct irq_data *data)
{
	struct nct6694 *ddata = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	guard(spinlock_irqsave)(&ddata->irq_lock);

	ddata->irq_enable |= BIT(hwirq);
}

static void nct6694_irq_disable(struct irq_data *data)
{
	struct nct6694 *ddata = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	guard(spinlock_irqsave)(&ddata->irq_lock);

	ddata->irq_enable &= ~BIT(hwirq);
}

static const struct irq_chip nct6694_irq_chip = {
	.name = "nct6694-irq",
	.flags = IRQCHIP_SKIP_SET_WAKE,
	.irq_enable = nct6694_irq_enable,
	.irq_disable = nct6694_irq_disable,
};

static int nct6694_irq_domain_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct nct6694 *ddata = d->host_data;

	irq_set_chip_data(irq, ddata);
	irq_set_chip_and_handler(irq, &nct6694_irq_chip, handle_simple_irq);

	return 0;
}

static void nct6694_irq_domain_unmap(struct irq_domain *d, unsigned int irq)
{
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops nct6694_irq_domain_ops = {
	.map	= nct6694_irq_domain_map,
	.unmap	= nct6694_irq_domain_unmap,
};

/**
 * nct6694_device_init() - Set up the resources shared by all transports
 * @dev: transport device whose driver data points to the struct nct6694
 *
 * Return: 0 on success or negative errno on failure.
 */
int nct6694_device_init(struct device *dev)
{
	struct nct6694 *ddata = dev_get_drvdata(dev);

	spin_lock_init(&ddata->irq_lock);

	ida_init(&ddata->gpio_ida);
	ida_init(&ddata->i2c_ida);
	ida_init(&ddata->canfd_ida);
	ida_init(&ddata->wdt_ida);

	ddata->domain = irq_domain_create_simple(NULL, NCT6694_NR_IRQS, 0,
						 &nct6694_irq_domain_ops,
						 ddata);
	if (!ddata->domain)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_GPL(nct6694_device_init);

/**
 * nct6694_device_exit() - Release the resources set up by nct6694_device_init()
 * @dev: transport device passed to nct6694_device_init()
 */
void nct6694_device_exit(struct device *dev)
{
	struct nct6694 *ddata = dev_get_drvdata(dev);

	ida_destroy(&ddata->wdt_ida);
	ida_destroy(&ddata->canfd_ida);
	ida_destroy(&ddata->i2c_ida);
	ida_destroy(&ddata->gpio_ida);
	irq_domain_remove(ddata->domain);
}
EXPORT_SYMBOL_GPL(nct6694_device_exit);

MODULE_DESCRIPTION("Nuvoton NCT6694 core driver");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
