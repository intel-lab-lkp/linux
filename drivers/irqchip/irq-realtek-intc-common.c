// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause)
/*
 * Realtek DHC SoCs interrupt controller driver
 *
 * Copyright (c) 2023 Realtek Semiconductor Corporation
 */

#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "irq-realtek-intc-common.h"

struct realtek_intc_data;

static inline unsigned int realtek_intc_get_ints(struct realtek_intc_data *data)
{
	return readl(data->base + data->info->isr_offset);
}

static inline void realtek_intc_clear_ints_bit(struct realtek_intc_data *data, int bit)
{
	writel(BIT(bit) & ~1, data->base + data->info->isr_offset);
}

static inline unsigned int realtek_intc_get_inte(struct realtek_intc_data *data)
{
	unsigned int val;

	raw_spin_lock(&data->lock);
	val = readl(data->base + data->info->scpu_int_en_offset);
	raw_spin_unlock(&data->lock);

	return val;
}

static void realtek_intc_handler(struct irq_desc *desc)
{
	struct realtek_intc_subset_data *subset_data = irq_desc_get_handler_data(desc);
	struct realtek_intc_data *data = subset_data->common;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 ints, inte, mask;
	int irq;

	chained_irq_enter(chip, desc);

	ints = realtek_intc_get_ints(data) & subset_data->cfg->ints_mask;
	inte = realtek_intc_get_inte(data);

	while (ints) {
		irq = __ffs(ints);
		ints &= ~BIT(irq);

		mask = data->info->isr_to_scpu_int_en_mask[irq];
		if (mask != IRQ_ALWAYS_ENABLED && !(inte & mask))
			continue;

		generic_handle_irq(irq_find_mapping(data->domain, irq));
		realtek_intc_clear_ints_bit(data, irq);
	}

	chained_irq_exit(chip, desc);
}

static void realtek_intc_mask_irq(struct irq_data *data)
{
	struct realtek_intc_data *intc_data = irq_data_get_irq_chip_data(data);

	writel(BIT(data->hwirq), intc_data->base + intc_data->info->isr_offset);
}

static void realtek_intc_unmask_irq(struct irq_data *data)
{
	struct realtek_intc_data *intc_data = irq_data_get_irq_chip_data(data);

	writel(BIT(data->hwirq), intc_data->base + intc_data->info->umsk_isr_offset);
}

static void realtek_intc_enable_irq(struct irq_data *data)
{
	struct realtek_intc_data *intc_data = irq_data_get_irq_chip_data(data);
	u32 scpu_int_en, mask;

	mask = intc_data->info->isr_to_scpu_int_en_mask[data->hwirq];
	if (!mask)
		return;

	raw_spin_lock(&intc_data->lock);
	scpu_int_en = readl(intc_data->base + intc_data->info->scpu_int_en_offset);
	scpu_int_en |= mask;
	writel(scpu_int_en, intc_data->base + intc_data->info->umsk_isr_offset);
	raw_spin_unlock(&intc_data->lock);
}

static void realtek_intc_disable_irq(struct irq_data *data)
{
	struct realtek_intc_data *intc_data = irq_data_get_irq_chip_data(data);
	u32 scpu_int_en, mask;

	mask = intc_data->info->isr_to_scpu_int_en_mask[data->hwirq];
	if (!mask)
		return;

	raw_spin_lock(&intc_data->lock);
	scpu_int_en = readl(intc_data->base + intc_data->info->scpu_int_en_offset);
	scpu_int_en &= ~mask;
	writel(scpu_int_en, intc_data->base + intc_data->info->umsk_isr_offset);
	raw_spin_unlock(&intc_data->lock);
}

static struct irq_chip realtek_intc_chip = {
	.name		  = "realtek-intc",
	.irq_mask	  = realtek_intc_mask_irq,
	.irq_unmask	  = realtek_intc_unmask_irq,
	.irq_enable	  = realtek_intc_enable_irq,
	.irq_disable	  = realtek_intc_disable_irq,
};

static int realtek_intc_domain_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	struct realtek_intc_data *data = d->host_data;

	irq_set_chip_and_handler(irq, &realtek_intc_chip, handle_level_irq);
	irq_set_chip_data(irq, data);
	irq_set_probe(irq);

	return 0;
}

static const struct irq_domain_ops realtek_intc_domain_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = realtek_intc_domain_map,
};

static int realtek_intc_subset(struct device_node *node, struct realtek_intc_data *data, int index)
{
	struct realtek_intc_subset_data *subset_data = &data->subset_data[index];
	const struct realtek_intc_subset_cfg *cfg = &data->info->cfg[index];
	int irq;

	irq = irq_of_parse_and_map(node, index);
	if (irq <= 0)
		return irq;

	subset_data->common = data;
	subset_data->cfg = cfg;
	subset_data->parent_irq = irq;
	irq_set_chained_handler_and_data(irq, realtek_intc_handler, subset_data);

	return 0;
}

int realtek_intc_probe(struct platform_device *pdev, const struct realtek_intc_info *info)
{
	struct realtek_intc_data *data;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	int ret, i;

	data = devm_kzalloc(dev, struct_size(data, subset_data, info->cfg_num), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->base = of_iomap(node, 0);
	if (!data->base)
		return -ENOMEM;

	data->info = info;

	raw_spin_lock_init(&data->lock);

	data->domain = irq_domain_add_linear(node, 32, &realtek_intc_domain_ops, data);
	if (!data->domain)
		return -ENOMEM;

	data->subset_data_num = info->cfg_num;
	for (i = 0; i < info->cfg_num; i++) {
		ret = realtek_intc_subset(node, data, i);
		WARN(ret, "failed to init subset %d: %d", i, ret);
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, data);

	return 0;
}
EXPORT_SYMBOL(realtek_intc_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Realtek DHC SoC Interrupt Controller Driver");
