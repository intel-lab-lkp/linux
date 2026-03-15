// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2009, Code Aurora Forum. All rights reserved.
 * Copyright (c) 2024, Htc Leo Revival Project
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/timer.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/cacheflush.h>

#include <asm/exception.h>
#include <asm/irq.h>

#include <dt-bindings/interrupt-controller/qcom-vic.h>

static u32 msm_irq_smsm_wake_enable[2];
struct msm_irq_shadow {
	u32 int_en[2];
	u32 int_type;
	u32 int_polarity;
	u32 int_select;
};

static struct msm_irq_shadow *msm_irq_shadow_reg;
static u32 *msm_irq_idle_disable;

struct msm_irq_map {
	u32 irq;
	u8 smsm;
};

struct vic_device {
	void __iomem *base;
	struct irq_domain *domain;
	u32				msm_nr_irqs;
	u32				nr_vic_irqs;
	u32				nr_gpio_irqs;
	u32				vic_num_regs;
	struct msm_irq_map *irq_map;
	int irq_map_count;
};

static struct vic_device vic_data;

static int msm_irq_to_smsm(u32 irq)
{
	int i;

	if (!vic_data.irq_map)
		return -EINVAL;

	for (i = 0; i < vic_data.irq_map_count; i++) {
		if (vic_data.irq_map[i].irq == irq)
			return vic_data.irq_map[i].smsm;
		}

	return -ENOENT;
}

void set_irq_flags(unsigned int irq, unsigned int iflags);

void set_irq_flags(unsigned int irq, unsigned int iflags)
{
	unsigned long clr = 0, set = IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN;

	if (irq >= vic_data.msm_nr_irqs) {
		pr_err("Trying to set irq flags for IRQ%d\n", irq);
		return;
	}

	if (iflags & IRQF_VALID)
		clr |= IRQ_NOREQUEST;
	if (iflags & IRQF_PROBE)
		clr |= IRQ_NOPROBE;
	if (!(iflags & IRQF_NOAUTOEN))
		clr |= IRQ_NOAUTOEN;
	/* Order is clear bits in "clr" then set bits in "set" */
	irq_modify_status(irq, clr, set & ~clr);
}
EXPORT_SYMBOL_GPL(set_irq_flags);

static inline void msm_irq_write_all_regs(void __iomem *base, unsigned int val)
{
	for (int i = 0; i < vic_data.vic_num_regs; i++)
		writel(val, base + (i * 4));
}

static void msm_irq_ack(struct irq_data *d)
{
	void __iomem *reg = VIC_INT_TO_REG_ADDR(vic_data.base + VIC_INT_CLEAR0, d->irq);

	writel(1 << (d->irq & 31), reg);
}

static void msm_irq_mask(struct irq_data *d)
{
	void __iomem *reg = VIC_INT_TO_REG_ADDR(vic_data.base + VIC_INT_ENCLEAR0, d->irq);
	unsigned int index = VIC_INT_TO_REG_INDEX(d->irq);
	u32 mask = 1UL << (d->irq & 31);
	int smsm_irq = msm_irq_to_smsm(d->irq);

	msm_irq_shadow_reg[index].int_en[0] &= ~mask;
	writel(mask, reg);
	if (smsm_irq == 0) {
		msm_irq_idle_disable[index] &= ~mask;
	} else {
		mask = 1UL << (smsm_irq - 1);
		msm_irq_smsm_wake_enable[0] &= ~mask;
	}
}

static void msm_irq_unmask(struct irq_data *d)
{
	void __iomem *reg = VIC_INT_TO_REG_ADDR(vic_data.base + VIC_INT_ENSET0, d->irq);
	unsigned int index = VIC_INT_TO_REG_INDEX(d->irq);
	u32 mask = 1UL << (d->irq & 31);
	int smsm_irq = msm_irq_to_smsm(d->irq);

	msm_irq_shadow_reg[index].int_en[0] |= mask;
	writel(mask, reg);

	if (smsm_irq == 0) {
		msm_irq_idle_disable[index] |= mask;
	} else {
		mask = 1UL << (smsm_irq - 1);
		msm_irq_smsm_wake_enable[0] |= mask;
	}
}

static int msm_irq_set_wake(struct irq_data *d, unsigned int on)
{
	unsigned int index = VIC_INT_TO_REG_INDEX(d->irq);
	u32 mask = 1UL << (d->irq & 31);
	int smsm_irq = msm_irq_to_smsm(d->irq);

	if (smsm_irq == 0) {
		pr_err("%s: bad wakeup irq %d\n", __func__, d->irq);
		return -EINVAL;
	}
	if (on)
		msm_irq_shadow_reg[index].int_en[1] |= mask;
	else
		msm_irq_shadow_reg[index].int_en[1] &= ~mask;

	if (smsm_irq == SMSM_FAKE_IRQ)
		return 0;

	mask = 1UL << (smsm_irq - 1);
	if (on)
		msm_irq_smsm_wake_enable[1] |= mask;
	else
		msm_irq_smsm_wake_enable[1] &= ~mask;
	return 0;
}

static int msm_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	void __iomem *treg = VIC_INT_TO_REG_ADDR(vic_data.base + VIC_INT_TYPE0, d->irq);
	void __iomem *preg = VIC_INT_TO_REG_ADDR(vic_data.base + VIC_INT_POLARITY0, d->irq);
	unsigned int index = VIC_INT_TO_REG_INDEX(d->irq);
	int b = 1 << (d->irq & 31);
	u32 polarity;
	u32 type;

	polarity = msm_irq_shadow_reg[index].int_polarity;
	if (flow_type & (IRQF_TRIGGER_FALLING | IRQF_TRIGGER_LOW))
		polarity |= b;
	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_HIGH))
		polarity &= ~b;
	writel(polarity, preg);
	msm_irq_shadow_reg[index].int_polarity = polarity;

	type = msm_irq_shadow_reg[index].int_type;
	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) {
		type |= b;
		irq_set_handler_locked(d, handle_edge_irq);
	}
	if (flow_type & (IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW)) {
		type &= ~b;
		irq_set_handler_locked(d, handle_level_irq);
	}
	writel(type, treg);
	msm_irq_shadow_reg[index].int_type = type;
	return 0;
}

static inline void msm_vic_handle_irq(void __iomem *base_addr, struct pt_regs
		*regs)
{
	u32 irqnr;

	do {
		/* VIC_IRQ_VEC_RD has irq# or old irq# if the irq has been handled
		 * VIC_IRQ_VEC_PEND_RD has irq# or -1 if none pending *but* if you
		 * just read VIC_IRQ_VEC_PEND_RD you never get the first irq for some reason
		 */
		irqnr = readl_relaxed(base_addr + VIC_IRQ_VEC_RD);
		irqnr = readl_relaxed(base_addr + VIC_IRQ_VEC_PEND_RD);
		if (irqnr == -1)
			break;
		handle_IRQ(irqnr, regs);
	} while (1);
}

/* enable imprecise aborts */
static inline void local_cpsie_enable(void)
{
	asm volatile("cpsie a" : : : "memory");
}

static void __exception_irq_entry vic_handle_irq(struct pt_regs *regs)
{
	local_cpsie_enable();// local_abt_enable()?
	msm_vic_handle_irq(vic_data.base, regs);
}

static struct irq_chip msm_irq_chip = {
	.name          = "msm",
	.irq_disable   = msm_irq_mask,
	.irq_ack       = msm_irq_ack,
	.irq_mask      = msm_irq_mask,
	.irq_unmask    = msm_irq_unmask,
	.irq_set_wake  = msm_irq_set_wake,
	.irq_set_type  = msm_irq_set_type,
};

static int msm_vic_parse_irq_mapping(struct device_node *np, struct msm_irq_map **map, int *count)
{
	const __be32 *prop;
	int len, i;

	prop = of_get_property(np, "irq-mapping", &len);
	if (!prop) {
		pr_err("%s: No irq-mapping property in device tree\n", __func__);
		return -ENODEV;
	}

    /* Each entry is 2 u32 values: irq + smsm */
	*count = len / (2 * sizeof(u32));

	*map = kzalloc((*count) * sizeof(**map), GFP_KERNEL);
	if (!*map) {
		pr_err("%s: Failed to allocate memory for irq_map\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < *count; i++) {
		(*map)[i].irq = be32_to_cpu(prop[i * 2]);
		(*map)[i].smsm = be32_to_cpu(prop[i * 2 + 1]);
	}

	return 0;
}

static int __init msm_init_irq(struct device_node *intc, struct device_node *parent)
{
	int ret;

	vic_data.base = of_iomap(intc, 0);
	if (WARN_ON(!vic_data.base))
		return -EIO;
	ret = of_property_read_u32(intc, "num-irqs", &vic_data.nr_vic_irqs);
	if (ret) {
		pr_err("%s: failed to read num-irqs ret=%d\n", __func__, ret);
		return ret;
	}

	ret = of_property_read_u32(intc, "num-gpio-irqs", &vic_data.nr_gpio_irqs);
	if (ret) {
		pr_err("%s: failed to read num-gpio-irqs ret=%d\n", __func__, ret);
		return ret;
	}

	ret = msm_vic_parse_irq_mapping(intc, &vic_data.irq_map, &vic_data.irq_map_count);
	if (ret) {
		pr_err("Failed to parse irq-mapping\n");
		return ret;
	}

	ret = of_property_read_u32(intc, "vic-num-regs", &vic_data.vic_num_regs);
	if (ret) {
		pr_err("Failed to parse vic-num-regs\n");
		return ret;
	}

	msm_irq_shadow_reg = kcalloc(vic_data.vic_num_regs,
				     sizeof(*msm_irq_shadow_reg),
					 GFP_KERNEL);

	msm_irq_idle_disable = kcalloc(vic_data.vic_num_regs,
				       sizeof(*msm_irq_idle_disable),
				       GFP_KERNEL);

	if (!msm_irq_shadow_reg || !msm_irq_idle_disable)
		return -ENOMEM;

	vic_data.msm_nr_irqs = vic_data.nr_vic_irqs * 2 + vic_data.nr_gpio_irqs;

	ret = irq_alloc_descs(-1, 0, vic_data.nr_vic_irqs, 0);
	if (ret < 0)
		pr_warn("Couldn't allocate IRQ numbers\n");
	/* select level interrupts */
	msm_irq_write_all_regs(vic_data.base + VIC_INT_TYPE0, 0);

	/* select highlevel interrupts */
	msm_irq_write_all_regs(vic_data.base + VIC_INT_POLARITY0, 0);

	/* select IRQ for all INTs */
	msm_irq_write_all_regs(vic_data.base + VIC_INT_SELECT0, 0);

	/* disable all INTs */
	msm_irq_write_all_regs(vic_data.base + VIC_INT_EN0, 0);

	/* don't use vic */
	writel(0, vic_data.base + VIC_CONFIG);

	/* enable interrupt controller */
	writel(3, vic_data.base + VIC_INT_MASTEREN);

	for (int n = 0; n < vic_data.msm_nr_irqs; n++) {
		irq_set_chip_and_handler(n, &msm_irq_chip, handle_level_irq);
		set_irq_flags(n, IRQF_VALID);
	}

	/* Ready to receive interrupts */
	set_handle_irq(vic_handle_irq);

	vic_data.domain = irq_domain_create_legacy
		(of_fwnode_handle(intc),
		vic_data.nr_vic_irqs,
		0, 0,
		&irq_domain_simple_ops,
		&vic_data);
	if (!vic_data.domain)
		pr_err("%s: failed to register irq domain\n", __func__);
	return 0;
}

IRQCHIP_DECLARE(qcom_msm_vic, "qcom,msm-vic", msm_init_irq);