// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Nuvoton NCT6694 MFD core driver.
 *
 * This provides common registration for IRQ domain, IDA pools,
 * and MFD sub-devices shared by all transport drivers (USB, HIF).
 */

#include <linux/idr.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/nct6694.h>
#include <linux/module.h>
#include <linux/spinlock.h>

static void nct6694_irq_enable(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	guard(spinlock_irqsave)(&nct6694->irq_lock);

	nct6694->irq_enable |= BIT(hwirq);
}

static void nct6694_irq_disable(struct irq_data *data)
{
	struct nct6694 *nct6694 = irq_data_get_irq_chip_data(data);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	guard(spinlock_irqsave)(&nct6694->irq_lock);

	nct6694->irq_enable &= ~BIT(hwirq);
}

static const struct irq_chip nct6694_irq_chip = {
	.name = "nct6694-irq",
	.flags = IRQCHIP_SKIP_SET_WAKE,
	.irq_enable = nct6694_irq_enable,
	.irq_disable = nct6694_irq_disable,
};

static int nct6694_irq_domain_map(struct irq_domain *d, unsigned int irq,
				  irq_hw_number_t hw)
{
	struct nct6694 *nct6694 = d->host_data;

	irq_set_chip_data(irq, nct6694);
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
 * nct6694_core_probe() - Register IRQ domain, IDAs, and MFD sub-devices
 * @dev: parent device (USB interface or platform device)
 * @nct6694: initialized nct6694 structure with transport callbacks set
 *
 * This function completes the common probe steps shared by all transport
 * drivers: IRQ domain creation, IDA initialization, and MFD cell registration.
 *
 * The caller must have already set nct6694->dev, nct6694->priv,
 * nct6694->read_msg, and nct6694->write_msg before calling this.
 *
 * Return: 0 on success or negative errno on failure.
 */
int nct6694_core_probe(struct device *dev, struct nct6694 *nct6694,
		       const struct mfd_cell *cells, int n_cells)
{
	int ret;

	spin_lock_init(&nct6694->irq_lock);

	ida_init(&nct6694->gpio_ida);
	ida_init(&nct6694->i2c_ida);
	ida_init(&nct6694->canfd_ida);
	ida_init(&nct6694->wdt_ida);

	nct6694->domain = irq_domain_create_simple(NULL, NCT6694_NR_IRQS, 0,
						   &nct6694_irq_domain_ops,
						   nct6694);
	if (!nct6694->domain) {
		ret = -ENODEV;
		goto err_ida;
	}

	ret = mfd_add_hotplug_devices(dev, cells, n_cells);
	if (ret)
		goto err_domain;

	return 0;

err_domain:
	irq_domain_remove(nct6694->domain);
err_ida:
	ida_destroy(&nct6694->wdt_ida);
	ida_destroy(&nct6694->canfd_ida);
	ida_destroy(&nct6694->i2c_ida);
	ida_destroy(&nct6694->gpio_ida);
	return ret;
}
EXPORT_SYMBOL_GPL(nct6694_core_probe);

/**
 * nct6694_core_remove() - Unregister MFD sub-devices and free core resources
 * @nct6694: nct6694 structure previously passed to nct6694_core_probe()
 */
void nct6694_core_remove(struct nct6694 *nct6694)
{
	mfd_remove_devices(nct6694->dev);
	irq_domain_remove(nct6694->domain);
	ida_destroy(&nct6694->wdt_ida);
	ida_destroy(&nct6694->canfd_ida);
	ida_destroy(&nct6694->i2c_ida);
	ida_destroy(&nct6694->gpio_ida);
}
EXPORT_SYMBOL_GPL(nct6694_core_remove);

MODULE_DESCRIPTION("Nuvoton NCT6694 MFD core driver");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
