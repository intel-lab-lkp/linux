// SPDX-License-Identifier: GPL-2.0
/*
 * SH7751 based board IRL encoder driver
 * (Renesas RTS7751R2D / IO DATA DEVICE LANDISK, USL-5P)
 *
 * Copyright (C) 2023 Yoshinori Sato
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define NUM_IRQ 15
#define EXTIRQ_BASE 16

struct sh7751irl_intc_priv {
	void __iomem *base;
	struct irq_domain *irq_domain;
	int width;
	int type;
	int nr_irq;
	u32 enable_map[NUM_IRQ];
};

enum {type_enable, type_mask};

static inline u32 get_reg(void *addr, int w)
{
	switch (w) {
	case 8:
		return __raw_readb(addr);
	case 16:
		return __raw_readw(addr);
	case 32:
		return __raw_readl(addr);
	}
	return 0;
}

static inline void set_reg(void *addr, int w, u32 val)
{
	switch (w) {
	case 8:
		__raw_writeb(val, addr);
		break;
	case 16:
		__raw_writew(val, addr);
		break;
	case 32:
		__raw_writel(val, addr);
		break;
	}
}

static inline struct sh7751irl_intc_priv *irq_data_to_priv(struct irq_data *data)
{
	return data->domain->host_data;
}

static inline u32 set_reset_bit(int val, u32 in, int bit, int type)
{
	val &= 1;
	if (type == type_mask)
		val ^= 1;
	in &= ~(1 << bit);
	return in | (val << bit);
}

static inline void mask_unmask(struct irq_data *data, int en)
{
	struct sh7751irl_intc_priv *priv = irq_data_to_priv(data);
	int irq = data->irq - EXTIRQ_BASE;
	u32 val;

	if (priv->nr_irq > irq && priv->enable_map[irq] < priv->width) {
		val = get_reg(priv->base, priv->width);
		val = set_reset_bit(en, val, priv->enable_map[irq], priv->type);
		set_reg(priv->base, priv->width, val);
	}
}

static void sh7751irl_intc_mask_irq(struct irq_data *data)
{
	mask_unmask(data, 0);
}

static void sh7751irl_intc_unmask_irq(struct irq_data *data)
{
	mask_unmask(data, 1);
}

static struct irq_chip sh7751irl_intc_chip = {
	.name		= "SH7751IRL-INTC",
	.irq_unmask	= sh7751irl_intc_unmask_irq,
	.irq_mask	= sh7751irl_intc_mask_irq,
};

static int sh7751irl_intc_map(struct irq_domain *h, unsigned int virq,
			       irq_hw_number_t hw_irq_num)
{
	irq_set_chip_and_handler(virq, &sh7751irl_intc_chip, handle_level_irq);
	irq_get_irq_data(virq)->chip_data = h->host_data;
	irq_modify_status(virq, IRQ_NOREQUEST, IRQ_NOPROBE);
	return 0;
}

static int sh7751irl_intc_translate(struct irq_domain *domain,
			       struct irq_fwspec *fwspec, unsigned long *hwirq,
			       unsigned int *type)
{
	if (fwspec->param[0] >= NUM_IRQ)
		return -EINVAL;

	switch (fwspec->param_count) {
	case 2:
		*type = fwspec->param[1];
		fallthrough;
	case 1:
		*hwirq = fwspec->param[0] + EXTIRQ_BASE;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct irq_domain_ops sh7751irl_intc_domain_ops = {
	.map = sh7751irl_intc_map,
	.translate = sh7751irl_intc_translate,
};

static int sh7751irl_init(struct device_node *node, struct device_node *parent)
{
	struct sh7751irl_intc_priv *priv;
	struct irq_domain *d;
	int ret = 0;
	int type = -1;
	u32 *p;
	unsigned int i, nr_input = 0;
	const char *type_str;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = of_iomap(node, 0);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto error;
	}
	of_property_read_u32(node, "renesas,width", &priv->width);
	if (priv->width != 8 && priv->width != 16 && priv->width != 32) {
		pr_err("%s Invalid register width.\n", node->name);
		ret = -EINVAL;
		goto error;
	}
	if (!of_property_read_string(node, "renesas,regtype", &type_str)) {
		if (strcasecmp("enable", type_str) == 0)
			type = type_enable;
		else if (strcasecmp("mask", type_str) == 0)
			type = type_mask;
	}
	if (type < 0) {
		pr_err("%pOFP: renesas,regtype Invalid register type (%s).\n", node, type_str);
		ret = -EINVAL;
		goto error;
	}
	priv->type = type;

	priv->nr_irq = of_property_count_u32_elems(node, "renesas,irqbit");
	if (priv->nr_irq < NUM_IRQ) {
		of_property_read_u32_array(node, "renesas,irqbit", priv->enable_map, priv->nr_irq);
		for (p = priv->enable_map, i = 0; i < priv->nr_irq; p++, i++) {
			if (*p != 0xffffffff)
				nr_input++;
		}
	}
	if (priv->nr_irq <= 0 || priv->nr_irq >= NUM_IRQ || nr_input > priv->width) {
		pr_err("%pOFP: renesas,irqbit Invalid register definition.\n", node);
		ret = -EINVAL;
		goto error;
	}
	d = irq_domain_add_tree(node, &sh7751irl_intc_domain_ops, priv);
	if (d == NULL) {
		pr_err("%pOFP: cannot initialize irq domain\n", node);
		ret = -ENOMEM;
		goto error;
	}
	priv->irq_domain = d;
	irq_domain_update_bus_token(d, DOMAIN_BUS_WIRED);
	pr_info("%pOFP: SH7751 External Interrupt encoder (input=%d)", node, nr_input);
	return 0;
error:
	kfree(priv);
	return ret;
}

IRQCHIP_DECLARE(renesas_sh7751_irl, "renesas,sh7751-irl-ext", sh7751irl_init);
