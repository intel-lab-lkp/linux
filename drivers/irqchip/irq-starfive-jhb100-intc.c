// SPDX-License-Identifier: GPL-2.0
/*
 * StarFive JHB100 External Interrupt Controller driver
 *
 * Copyright (C) 2023 StarFive Technology Co., Ltd.
 *
 * Author: Changhuang Liang <changhuang.liang@starfivetech.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define STARFIVE_INTC_SRC_TYPE(n)	(0x04 + ((n) * 0x20))
#define STARFIVE_INTC_SRC_CLEAR(n)	(0x10 + ((n) * 0x20))
#define STARFIVE_INTC_SRC_MASK(n)	(0x14 + ((n) * 0x20))
#define STARFIVE_INTC_SRC_INT(n)	(0x1c + ((n) * 0x20))

#define STARFIVE_INTC_TRIGGER_MASK	0x3
#define STARFIVE_INTC_TRIGGER_HIGH	0
#define STARFIVE_INTC_TRIGGER_LOW	1
#define STARFIVE_INTC_TRIGGER_POSEDGE	2
#define STARFIVE_INTC_TRIGGER_NEGEDGE	3

#define STARFIVE_INTC_NUM		2
#define STARFIVE_INTC_SRC_IRQ_NUM	32
#define STARFIVE_INTC_TYPE_NUM		16

struct starfive_irq_chip {
	void __iomem		*base;
	struct irq_domain	*domain;
	raw_spinlock_t		lock;
};

static void starfive_intc_mod(struct starfive_irq_chip *irqc, u32 reg,
			      u32 mask, u32 data)
{
	u32 value;

	value = ioread32(irqc->base + reg) & ~mask;
	data &= mask;
	data |= value;
	iowrite32(data, irqc->base + reg);
}

static void starfive_intc_bit_set(struct starfive_irq_chip *irqc,
				  u32 reg, u32 bit_mask)
{
	u32 value;

	value = ioread32(irqc->base + reg);
	value |= bit_mask;
	iowrite32(value, irqc->base + reg);
}

static void starfive_intc_bit_clear(struct starfive_irq_chip *irqc,
				    u32 reg, u32 bit_mask)
{
	u32 value;

	value = ioread32(irqc->base + reg);
	value &= ~bit_mask;
	iowrite32(value, irqc->base + reg);
}

static void starfive_intc_unmask(struct irq_data *d)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_chip_data(d);
	int i, bitpos;

	i = d->hwirq / STARFIVE_INTC_SRC_IRQ_NUM;
	bitpos = d->hwirq % STARFIVE_INTC_SRC_IRQ_NUM;

	raw_spin_lock(&irqc->lock);
	starfive_intc_bit_clear(irqc, STARFIVE_INTC_SRC_MASK(i), BIT(bitpos));
	raw_spin_unlock(&irqc->lock);
}

static void starfive_intc_mask(struct irq_data *d)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_chip_data(d);
	int i, bitpos;

	i = d->hwirq / STARFIVE_INTC_SRC_IRQ_NUM;
	bitpos = d->hwirq % STARFIVE_INTC_SRC_IRQ_NUM;

	raw_spin_lock(&irqc->lock);
	starfive_intc_bit_set(irqc, STARFIVE_INTC_SRC_MASK(i), BIT(bitpos));
	raw_spin_unlock(&irqc->lock);
}

static void starfive_intc_ack(struct irq_data *d)
{
	/* for handle_edge_irq, nothing to do */
}

static int starfive_intc_set_type(struct irq_data *d, unsigned int type)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_chip_data(d);
	u32 i, bitpos, ty_pos, ty_shift, tmp;

	i = d->hwirq / STARFIVE_INTC_SRC_IRQ_NUM;
	bitpos = d->hwirq % STARFIVE_INTC_SRC_IRQ_NUM;
	ty_pos = bitpos / STARFIVE_INTC_TYPE_NUM;
	ty_shift = (bitpos % STARFIVE_INTC_TYPE_NUM) * 2;

	switch (type) {
	case IRQF_TRIGGER_LOW:
		tmp = STARFIVE_INTC_TRIGGER_LOW << ty_shift;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQF_TRIGGER_HIGH:
		tmp = STARFIVE_INTC_TRIGGER_HIGH << ty_shift;
		irq_set_handler_locked(d, handle_level_irq);
		break;
	case IRQF_TRIGGER_FALLING:
		tmp = STARFIVE_INTC_TRIGGER_NEGEDGE << ty_shift;
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	case IRQF_TRIGGER_RISING:
		tmp = STARFIVE_INTC_TRIGGER_POSEDGE << ty_shift;
		irq_set_handler_locked(d, handle_edge_irq);
		break;
	default:
		return -EINVAL;
	}

	raw_spin_lock(&irqc->lock);

	starfive_intc_mod(irqc, STARFIVE_INTC_SRC_TYPE(i) + 4 * ty_pos,
			  STARFIVE_INTC_TRIGGER_MASK << ty_shift, tmp);

	/* Once the type is updated, clear interrupt can help to reset the type value */
	starfive_intc_bit_set(irqc, STARFIVE_INTC_SRC_CLEAR(i), BIT(bitpos));
	starfive_intc_bit_clear(irqc, STARFIVE_INTC_SRC_CLEAR(i), BIT(bitpos));

	raw_spin_unlock(&irqc->lock);

	return 0;
}

static struct irq_chip intc_dev = {
	.name		= "StarFive JHB100 INTC",
	.irq_unmask	= starfive_intc_unmask,
	.irq_mask	= starfive_intc_mask,
	.irq_ack	= starfive_intc_ack,
	.irq_set_type	= starfive_intc_set_type,
};

static int starfive_intc_map(struct irq_domain *d, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_domain_set_info(d, irq, hwirq, &intc_dev, d->host_data,
			    handle_level_irq, NULL, NULL);

	return 0;
}

static const struct irq_domain_ops starfive_intc_domain_ops = {
	.xlate	= irq_domain_xlate_onecell,
	.map	= starfive_intc_map,
};

static void starfive_intc_irq_handler(struct irq_desc *desc)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_handler_data(&desc->irq_data);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long value;
	int hwirq, i;

	chained_irq_enter(chip, desc);

	for (i = 0; i < STARFIVE_INTC_NUM; i++) {
		value = ioread32(irqc->base + STARFIVE_INTC_SRC_INT(i));
		while (value) {
			hwirq = ffs(value) - 1;

			generic_handle_domain_irq(irqc->domain,
						  hwirq + i * STARFIVE_INTC_SRC_IRQ_NUM);

			starfive_intc_bit_set(irqc, STARFIVE_INTC_SRC_CLEAR(i), BIT(hwirq));
			starfive_intc_bit_clear(irqc, STARFIVE_INTC_SRC_CLEAR(i), BIT(hwirq));

			__clear_bit(hwirq, &value);
		}
	}

	chained_irq_exit(chip, desc);
}

static int starfive_intc_probe(struct platform_device *pdev, struct device_node *parent)
{
	struct device_node *intc = pdev->dev.of_node;
	struct starfive_irq_chip *irqc;
	struct reset_control *rst;
	struct clk *clk;
	int parent_irq;
	int ret;

	irqc = kzalloc_obj(*irqc);
	if (!irqc)
		return -ENOMEM;

	irqc->base = devm_platform_ioremap_resource(pdev, 0);
	if (!irqc->base) {
		dev_err(&pdev->dev, "unable to map registers\n");
		ret = -ENXIO;
		goto err_free;
	}

	rst = devm_reset_control_get_optional(&pdev->dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(&pdev->dev, "Unable to get reset control %pe\n", rst);
		ret = PTR_ERR(rst);
		goto err_free;
	}

	clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Unable to get and enable clock %pe\n", clk);
		ret = PTR_ERR(clk);
		goto err_free;
	}

	ret = reset_control_deassert(rst);
	if (ret)
		goto err_free;

	raw_spin_lock_init(&irqc->lock);

	irqc->domain = irq_domain_create_linear(of_fwnode_handle(intc),
						STARFIVE_INTC_SRC_IRQ_NUM * STARFIVE_INTC_NUM,
						&starfive_intc_domain_ops, irqc);
	if (!irqc->domain) {
		dev_err(&pdev->dev, "Unable to create IRQ domain\n");
		ret = -EINVAL;
		goto err_reset_assert;
	}

	parent_irq = of_irq_get(intc, 0);
	if (parent_irq < 0) {
		dev_err(&pdev->dev, "Failed to get main IRQ: %d\n", parent_irq);
		ret = parent_irq;
		goto err_remove_domain;
	}

	irq_set_chained_handler_and_data(parent_irq, starfive_intc_irq_handler,
					 irqc);

	dev_info(&pdev->dev, "Interrupt controller register, nr_irqs %d\n",
		 STARFIVE_INTC_SRC_IRQ_NUM * STARFIVE_INTC_NUM);

	return 0;

err_remove_domain:
	irq_domain_remove(irqc->domain);
err_reset_assert:
	reset_control_assert(rst);
err_free:
	kfree(irqc);
	return ret;
}

IRQCHIP_PLATFORM_DRIVER_BEGIN(starfive_intc)
IRQCHIP_MATCH("starfive,jhb100-intc", starfive_intc_probe)
IRQCHIP_PLATFORM_DRIVER_END(starfive_intc)

MODULE_DESCRIPTION("StarFive JHB100 External Interrupt Controller");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Changhuang Liang <changhuang.liang@starfivetech.com>");
