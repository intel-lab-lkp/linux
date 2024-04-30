// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Microchip LAN966x outbound interrupt controller
 *
 * Copyright (c) 2024 Technology Inc. and its subsidiaries.
 *
 * Authors:
 *	Horatiu Vultur <horatiu.vultur@microchip.com>
 *	Clément Léger <clement.leger@bootlin.com>
 *	Herve Codina <herve.codina@bootlin.com>
 */

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqchip.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>

struct lan966x_oic_chip_regs {
	int reg_off_ena_set;
	int reg_off_ena_clr;
	int reg_off_sticky;
	int reg_off_ident;
	int reg_off_map;
};

struct lan966x_oic_data {
	struct irq_domain *domain;
	void __iomem *regs;
	int irq;
};

#define LAN966X_OIC_NR_IRQ 86

/* Interrupt sticky status */
#define LAN966X_OIC_INTR_STICKY		0x30
#define LAN966X_OIC_INTR_STICKY1	0x34
#define LAN966X_OIC_INTR_STICKY2	0x38

/* Interrupt enable */
#define LAN966X_OIC_INTR_ENA		0x48
#define LAN966X_OIC_INTR_ENA1		0x4c
#define LAN966X_OIC_INTR_ENA2		0x50

/* Atomic clear of interrupt enable */
#define LAN966X_OIC_INTR_ENA_CLR	0x54
#define LAN966X_OIC_INTR_ENA_CLR1	0x58
#define LAN966X_OIC_INTR_ENA_CLR2	0x5c

/* Atomic set of interrupt */
#define LAN966X_OIC_INTR_ENA_SET	0x60
#define LAN966X_OIC_INTR_ENA_SET1	0x64
#define LAN966X_OIC_INTR_ENA_SET2	0x68

/* Mapping of source to destination interrupts (_n = 0..8) */
#define LAN966X_OIC_DST_INTR_MAP(_n)	0x78
#define LAN966X_OIC_DST_INTR_MAP1(_n)	0x9c
#define LAN966X_OIC_DST_INTR_MAP2(_n)	0xc0

/* Currently active interrupt sources per destination (_n = 0..8) */
#define LAN966X_OIC_DST_INTR_IDENT(_n)	0xe4
#define LAN966X_OIC_DST_INTR_IDENT1(_n)	0x108
#define LAN966X_OIC_DST_INTR_IDENT2(_n)	0x12c

static unsigned int lan966x_oic_irq_startup(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	struct lan966x_oic_chip_regs *chip_regs = gc->private;
	u32 map;

	irq_gc_lock(gc);

	/* Map the source interrupt to the destination */
	map = irq_reg_readl(gc, chip_regs->reg_off_map);
	map |= data->mask;
	irq_reg_writel(gc, map, chip_regs->reg_off_map);

	irq_gc_unlock(gc);

	ct->chip.irq_ack(data);
	ct->chip.irq_unmask(data);

	return 0;
}

static void lan966x_oic_irq_shutdown(struct irq_data *data)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(data);
	struct irq_chip_type *ct = irq_data_get_chip_type(data);
	struct lan966x_oic_chip_regs *chip_regs = gc->private;
	u32 map;

	ct->chip.irq_mask(data);

	irq_gc_lock(gc);

	/* Unmap the interrupt */
	map = irq_reg_readl(gc, chip_regs->reg_off_map);
	map &= ~data->mask;
	irq_reg_writel(gc, map, chip_regs->reg_off_map);

	irq_gc_unlock(gc);
}

static int lan966x_oic_irq_set_type(struct irq_data *data, unsigned int flow_type)
{
	if (flow_type != IRQ_TYPE_LEVEL_HIGH) {
		pr_err("lan966x oic doesn't support flow type %d\n", flow_type);
		return -EINVAL;
	}

	return 0;
}

static void lan966x_oic_irq_handler_domain(struct irq_domain *d, u32 first_irq)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, first_irq);
	struct lan966x_oic_chip_regs *chip_regs = gc->private;
	unsigned long ident;
	unsigned int hwirq;

	ident = irq_reg_readl(gc, chip_regs->reg_off_ident);
	if (!ident)
		return;

	for_each_set_bit(hwirq, &ident, 32)
		generic_handle_domain_irq(d, hwirq + first_irq);
}

static void lan966x_oic_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *d = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);
	lan966x_oic_irq_handler_domain(d, 0);
	lan966x_oic_irq_handler_domain(d, 32);
	lan966x_oic_irq_handler_domain(d, 64);
	chained_irq_exit(chip, desc);
}

static struct lan966x_oic_chip_regs lan966x_oic_chip_regs[3] = {
	{
		.reg_off_ena_set = LAN966X_OIC_INTR_ENA_SET,
		.reg_off_ena_clr = LAN966X_OIC_INTR_ENA_CLR,
		.reg_off_sticky = LAN966X_OIC_INTR_STICKY,
		.reg_off_ident = LAN966X_OIC_DST_INTR_IDENT(0),
		.reg_off_map = LAN966X_OIC_DST_INTR_MAP(0),
	}, {
		.reg_off_ena_set = LAN966X_OIC_INTR_ENA_SET1,
		.reg_off_ena_clr = LAN966X_OIC_INTR_ENA_CLR1,
		.reg_off_sticky = LAN966X_OIC_INTR_STICKY1,
		.reg_off_ident = LAN966X_OIC_DST_INTR_IDENT1(0),
		.reg_off_map = LAN966X_OIC_DST_INTR_MAP1(0),
	}, {
		.reg_off_ena_set = LAN966X_OIC_INTR_ENA_SET2,
		.reg_off_ena_clr = LAN966X_OIC_INTR_ENA_CLR2,
		.reg_off_sticky = LAN966X_OIC_INTR_STICKY2,
		.reg_off_ident = LAN966X_OIC_DST_INTR_IDENT2(0),
		.reg_off_map = LAN966X_OIC_DST_INTR_MAP2(0),
	}
};

static void lan966x_oic_chip_init(struct lan966x_oic_data *lan966x_oic,
				  struct irq_chip_generic *gc,
				  struct lan966x_oic_chip_regs *chip_regs)
{
	gc->reg_base = lan966x_oic->regs;
	gc->chip_types[0].regs.enable = chip_regs->reg_off_ena_set;
	gc->chip_types[0].regs.disable = chip_regs->reg_off_ena_clr;
	gc->chip_types[0].regs.ack = chip_regs->reg_off_sticky;
	gc->chip_types[0].chip.irq_startup = lan966x_oic_irq_startup;
	gc->chip_types[0].chip.irq_shutdown = lan966x_oic_irq_shutdown;
	gc->chip_types[0].chip.irq_set_type = lan966x_oic_irq_set_type;
	gc->chip_types[0].chip.irq_mask = irq_gc_mask_disable_reg;
	gc->chip_types[0].chip.irq_unmask = irq_gc_unmask_enable_reg;
	gc->chip_types[0].chip.irq_ack = irq_gc_ack_set_bit;
	gc->private = chip_regs;

	/* Disable all interrupts handled by this chip */
	irq_reg_writel(gc, ~0, chip_regs->reg_off_ena_clr);
}

static void lan966x_oic_chip_exit(struct irq_chip_generic *gc)
{
	/* Disable and ack all interrupts handled by this chip */
	irq_reg_writel(gc, ~0, gc->chip_types[0].regs.disable);
	irq_reg_writel(gc, ~0, gc->chip_types[0].regs.ack);
}

static int lan966x_oic_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct lan966x_oic_data *lan966x_oic;
	struct device *dev = &pdev->dev;
	struct irq_chip_generic *gc;
	int ret;
	int i;

	lan966x_oic = devm_kmalloc(dev, sizeof(*lan966x_oic), GFP_KERNEL);
	if (!lan966x_oic)
		return -ENOMEM;

	lan966x_oic->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(lan966x_oic->regs))
		return dev_err_probe(dev, PTR_ERR(lan966x_oic->regs),
				     "failed to map resource\n");

	lan966x_oic->domain = irq_domain_alloc_linear(of_node_to_fwnode(node),
						      LAN966X_OIC_NR_IRQ,
						      &irq_generic_chip_ops, NULL);
	if (!lan966x_oic->domain) {
		dev_err(dev, "failed to create an IRQ domain\n");
		return -EINVAL;
	}

	lan966x_oic->irq = platform_get_irq(pdev, 0);
	if (lan966x_oic->irq < 0) {
		dev_err_probe(dev, lan966x_oic->irq, "failed to get the IRQ\n");
		goto err_domain_free;
	}

	ret = irq_alloc_domain_generic_chips(lan966x_oic->domain, 32, 1, "lan966x-oic",
					     handle_level_irq, 0, 0, 0);
	if (ret) {
		dev_err_probe(dev, ret, "failed to alloc irq domain gc\n");
		goto err_domain_free;
	}

	/* Init chips */
	BUILD_BUG_ON(DIV_ROUND_UP(LAN966X_OIC_NR_IRQ, 32) != ARRAY_SIZE(lan966x_oic_chip_regs));
	for (i = 0; i < ARRAY_SIZE(lan966x_oic_chip_regs); i++) {
		gc = irq_get_domain_generic_chip(lan966x_oic->domain, i * 32);
		lan966x_oic_chip_init(lan966x_oic, gc, &lan966x_oic_chip_regs[i]);
	}

	irq_set_chained_handler_and_data(lan966x_oic->irq, lan966x_oic_irq_handler,
					 lan966x_oic->domain);

	irq_domain_publish(lan966x_oic->domain);
	platform_set_drvdata(pdev, lan966x_oic);
	return 0;

err_domain_free:
	irq_domain_free(lan966x_oic->domain);
	return ret;
}

static void lan966x_oic_remove(struct platform_device *pdev)
{
	struct lan966x_oic_data *lan966x_oic = platform_get_drvdata(pdev);
	struct irq_chip_generic *gc;
	int i;

	for (i = 0; i < ARRAY_SIZE(lan966x_oic_chip_regs); i++) {
		gc = irq_get_domain_generic_chip(lan966x_oic->domain, i * 32);
		lan966x_oic_chip_exit(gc);
	}

	irq_set_chained_handler_and_data(lan966x_oic->irq, NULL, NULL);

	for (i = 0; i < LAN966X_OIC_NR_IRQ; i++)
		irq_dispose_mapping(irq_find_mapping(lan966x_oic->domain, i));

	irq_domain_unpublish(lan966x_oic->domain);

	for (i = 0; i < ARRAY_SIZE(lan966x_oic_chip_regs); i++) {
		gc = irq_get_domain_generic_chip(lan966x_oic->domain, i * 32);
		irq_remove_generic_chip(gc, ~0, 0, 0);
	}

	kfree(lan966x_oic->domain->gc);
	irq_domain_free(lan966x_oic->domain);
}

static const struct of_device_id lan966x_oic_of_match[] = {
	{ .compatible = "microchip,lan966x-oic" },
	{} /* sentinel */
};
MODULE_DEVICE_TABLE(of, lan966x_oic_of_match);

static struct platform_driver lan966x_oic_driver = {
	.probe = lan966x_oic_probe,
	.remove_new = lan966x_oic_remove,
	.driver = {
		.name = "lan966x-oic",
		.of_match_table = lan966x_oic_of_match,
	},
};
module_platform_driver(lan966x_oic_driver);

MODULE_AUTHOR("Herve Codina <herve.codina@bootlin.com>");
MODULE_DESCRIPTION("Microchip LAN966x OIC driver");
MODULE_LICENSE("GPL");
