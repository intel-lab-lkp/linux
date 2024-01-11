// SPDX-License-Identifier: GPL-2.0
/*
 * StarFive JH8100 External Interrupt Controller driver
 *
 * Copyright (C) 2023 StarFive Technology Co., Ltd.
 *
 * Author: Changhuang Liang <changhuang.liang@starfivetech.com>
 */

#define pr_fmt(fmt) "irq-starfive-jh8100: " fmt

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/reset.h>

#define STARFIVE_INTC_SRC0_CLEAR	0x10
#define STARFIVE_INTC_SRC0_MASK		0x14
#define STARFIVE_INTC_SRC0_INT		0x1c

#define STARFIVE_INTC_SRC_IRQ_NUM	32

struct starfive_irq_chip {
	void __iomem *base;
	struct irq_domain *root_domain;
	struct clk *clk;
	struct reset_control *rst;
};

static void starfive_intc_mod(struct starfive_irq_chip *irqc, u32 reg, u32 mask, u32 data)
{
	u32 value;

	value = ioread32(irqc->base + reg) & ~mask;
	data &= mask;
	data |= value;
	iowrite32(data, irqc->base + reg);
}

static void starfive_intc_unmask(struct irq_data *d)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_chip_data(d);

	starfive_intc_mod(irqc, STARFIVE_INTC_SRC0_MASK, BIT(d->hwirq), 0);
}

static void starfive_intc_mask(struct irq_data *d)
{
	struct starfive_irq_chip *irqc = irq_data_get_irq_chip_data(d);

	starfive_intc_mod(irqc, STARFIVE_INTC_SRC0_MASK, BIT(d->hwirq), BIT(d->hwirq));
}

static struct irq_chip intc_dev = {
	.name = "starfive jh8100 intc",
	.irq_unmask = starfive_intc_unmask,
	.irq_mask = starfive_intc_mask,
};

static int starfive_intc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hwirq)
{
	irq_domain_set_info(d, irq, hwirq, &intc_dev, d->host_data,
			    handle_level_irq, NULL, NULL);

	return 0;
}

static const struct irq_domain_ops starfive_intc_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = starfive_intc_map,
};

static void starfive_intc_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct starfive_irq_chip *irqc = irq_data_get_irq_handler_data(&desc->irq_data);
	unsigned long value = 0;
	int hwirq;

	chained_irq_enter(chip, desc);

	value = ioread32(irqc->base + STARFIVE_INTC_SRC0_INT);
	while (value) {
		hwirq = ffs(value) - 1;

		generic_handle_domain_irq(irqc->root_domain, hwirq);

		starfive_intc_mod(irqc, STARFIVE_INTC_SRC0_CLEAR, BIT(hwirq), BIT(hwirq));
		starfive_intc_mod(irqc, STARFIVE_INTC_SRC0_CLEAR, BIT(hwirq), 0);

		clear_bit(hwirq, &value);
	}

	chained_irq_exit(chip, desc);
}

static int __init starfive_intc_init(struct device_node *intc,
				     struct device_node *parent)
{
	struct starfive_irq_chip *irqc;
	int ret;
	int parent_irq;

	irqc = kzalloc(sizeof(*irqc), GFP_KERNEL);
	if (!irqc)
		return -ENOMEM;

	irqc->base = of_iomap(intc, 0);
	if (!irqc->base) {
		pr_err("Unable to map IC registers\n");
		ret = -ENXIO;
		goto err_free;
	}

	irqc->rst = of_reset_control_get_by_index(intc, 0);
	if (IS_ERR(irqc->rst)) {
		pr_err("Unable to get reset control\n");
		ret = PTR_ERR(irqc->rst);
		goto err_unmap;
	}

	irqc->clk = of_clk_get(intc, 0);
	if (IS_ERR(irqc->clk)) {
		pr_err("Unable to get clock\n");
		ret = PTR_ERR(irqc->clk);
		goto err_rst;
	}

	ret = reset_control_deassert(irqc->rst);
	if (ret)
		goto err_clk;

	ret = clk_prepare_enable(irqc->clk);
	if (ret)
		goto err_clk;

	irqc->root_domain = irq_domain_add_linear(intc, STARFIVE_INTC_SRC_IRQ_NUM,
						  &starfive_intc_domain_ops, irqc);
	if (!irqc->root_domain) {
		pr_err("Unable to create IRQ domain\n");
		ret = -EINVAL;
		goto err_clk;
	}

	parent_irq = of_irq_get(intc, 0);
	if (parent_irq < 0) {
		pr_err("Failed to get main IRQ: %d\n", parent_irq);
		ret = parent_irq;
		goto err_clk;
	}

	irq_set_chained_handler_and_data(parent_irq, starfive_intc_irq_handler, irqc);

	pr_info("Interrupt controller register, nr_irqs %d\n", STARFIVE_INTC_SRC_IRQ_NUM);

	return 0;

err_clk:
	clk_put(irqc->clk);
err_rst:
	reset_control_put(irqc->rst);
err_unmap:
	iounmap(irqc->base);
err_free:
	kfree(irqc);
	return ret;
}

IRQCHIP_PLATFORM_DRIVER_BEGIN(starfive_intc)
IRQCHIP_MATCH("starfive,jh8100-intc", starfive_intc_init)
IRQCHIP_PLATFORM_DRIVER_END(starfive_intc)

MODULE_DESCRIPTION("StarFive JH8100 External Interrupt Controller");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Changhuang Liang <changhuang.liang@starfivetech.com>");
