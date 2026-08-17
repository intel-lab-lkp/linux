// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx Versal Net SMMU CSR interrupt controller driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Vendor CSR block that gates the ARM SMMUv3 wired interrupts. Chains
 * off the single GIC parent line and demultiplexes the CSR status into
 * three children (eventq, gerror, priq), each enabled/acked
 * via SMMU_CSR_IER/IDR/ISR that the ARM SMMUv3 driver requests individually.
 */

#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/* Interrupt types */
#define SMMU_INTR_EVENT		BIT(0)
#define SMMU_INTR_GLOBAL	BIT(2)
#define SMMU_INTR_PRI		BIT(3)

/* Mask for all SMMU interrupts */
#define SMMU_INTR_ALL		(SMMU_INTR_EVENT | \
				 SMMU_INTR_GLOBAL | SMMU_INTR_PRI)

#define SMMU_CSR_ISR		0x24	/* Interrupt Status */
#define SMMU_CSR_IER		0x2c	/* Interrupt Enable */
#define SMMU_CSR_IDR		0x30	/* Interrupt Disable */

/**
 * struct xilinx_smmu_csr - SMMU CSR interrupt controller context
 * @base: MMIO base address of the CSR registers
 * @domain: IRQ domain for the child interrupts
 * @parent_irq: parent (GIC) IRQ this block is chained to
 * @lock: protects the SMMU_CSR_IER/IDR/ISR read and writes
 */
struct xilinx_smmu_csr {
	void __iomem		*base;
	struct irq_domain	*domain;
	int			parent_irq;
	raw_spinlock_t		lock;
};

enum xilinx_smmu_csr_irq {
	SMMU_CSR_IRQ_EVENTQ = 0,
	SMMU_CSR_IRQ_GERROR = 2,
	SMMU_CSR_IRQ_PRIQ = 3,
	SMMU_CSR_IRQ_NR,
};

static u32 xilinx_smmu_csr_hwirq_mask(irq_hw_number_t hwirq)
{
	if (hwirq >= SMMU_CSR_IRQ_NR)
		return 0;

	return BIT(hwirq) & SMMU_INTR_ALL;
}

static void xilinx_smmu_csr_irq_mask(struct irq_data *d)
{
	struct xilinx_smmu_csr *csr = irq_data_get_irq_chip_data(d);
	u32 mask = xilinx_smmu_csr_hwirq_mask(d->hwirq);

	if (!mask)
		return;

	raw_spin_lock(&csr->lock);
	writel(mask, csr->base + SMMU_CSR_IDR);
	raw_spin_unlock(&csr->lock);
}

static void xilinx_smmu_csr_irq_unmask(struct irq_data *d)
{
	struct xilinx_smmu_csr *csr = irq_data_get_irq_chip_data(d);
	u32 mask = xilinx_smmu_csr_hwirq_mask(d->hwirq);

	if (!mask)
		return;

	raw_spin_lock(&csr->lock);
	writel(mask, csr->base + SMMU_CSR_IER);
	raw_spin_unlock(&csr->lock);
}

static void xilinx_smmu_csr_irq_ack(struct irq_data *d)
{
	struct xilinx_smmu_csr *csr = irq_data_get_irq_chip_data(d);
	u32 mask = xilinx_smmu_csr_hwirq_mask(d->hwirq);

	if (!mask)
		return;

	raw_spin_lock(&csr->lock);
	writel(mask, csr->base + SMMU_CSR_ISR);
	raw_spin_unlock(&csr->lock);
}

static struct irq_chip xilinx_smmu_csr_chip = {
	.name		= "xlnx-smmu-csr",
	.irq_mask	= xilinx_smmu_csr_irq_mask,
	.irq_unmask	= xilinx_smmu_csr_irq_unmask,
	.irq_ack	= xilinx_smmu_csr_irq_ack,
};

static void xilinx_smmu_csr_irq_handler(struct irq_desc *desc)
{
	struct xilinx_smmu_csr *csr = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 status, pending;

	chained_irq_enter(chip, desc);
	raw_spin_lock(&csr->lock);
	status = readl(csr->base + SMMU_CSR_ISR);
	raw_spin_unlock(&csr->lock);

	/* Only service sources we support; SMMU_CSR_ISR latches raw status */
	pending = status & SMMU_INTR_ALL;

	while (pending) {
		irq_hw_number_t hwirq = __ffs(pending);
		int ret;

		ret = generic_handle_domain_irq(csr->domain, hwirq);
		if (ret) {
			raw_spin_lock(&csr->lock);
			writel(BIT(hwirq), csr->base + SMMU_CSR_ISR);
			raw_spin_unlock(&csr->lock);
			pr_err_ratelimited("xilinx-smmu-csr: Failed to handle domain IRQ %lu: %d\n",
					   hwirq, ret);
		}

		pending &= ~BIT(hwirq);
	}

	chained_irq_exit(chip, desc);
}

static int xilinx_smmu_csr_domain_map(struct irq_domain *d, unsigned int virq,
				      irq_hw_number_t hwirq)
{
	struct xilinx_smmu_csr *csr = d->host_data;

	if (!xilinx_smmu_csr_hwirq_mask(hwirq))
		return -EINVAL;

	irq_set_chip_and_handler(virq, &xilinx_smmu_csr_chip, handle_level_irq);
	irq_set_chip_data(virq, csr);
	irq_set_status_flags(virq, IRQ_LEVEL);

	return 0;
}

static const struct irq_domain_ops xilinx_smmu_csr_domain_ops = {
	.map	= xilinx_smmu_csr_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static int __init xilinx_smmu_csr_init(struct device_node *node,
				       struct device_node *parent)
{
	struct xilinx_smmu_csr *csr;
	int ret;

	if (WARN_ON_ONCE(!parent))
		return -EINVAL;

	if (irq_find_matching_fwnode(of_fwnode_handle(node),
				     DOMAIN_BUS_ANY))
		return -ENODEV;

	csr = kzalloc(sizeof(*csr), GFP_KERNEL);
	if (!csr)
		return -ENOMEM;

	raw_spin_lock_init(&csr->lock);

	csr->base = of_iomap(node, 0);
	if (!csr->base) {
		ret = -ENOMEM;
		goto free;
	}

	/* Start from a known state: all sources disabled, latches cleared. */
	writel(SMMU_INTR_ALL, csr->base + SMMU_CSR_IDR);
	writel(SMMU_INTR_ALL, csr->base + SMMU_CSR_ISR);

	csr->domain = irq_domain_create_linear(of_fwnode_handle(node), SMMU_CSR_IRQ_NR,
					       &xilinx_smmu_csr_domain_ops,
					       csr);
	if (!csr->domain) {
		pr_err("%pOF: failed to create irq domain\n", node);
		ret = -ENOMEM;
		goto unmap;
	}

	csr->parent_irq = irq_of_parse_and_map(node, 0);
	if (!csr->parent_irq) {
		pr_err("%pOF: failed to map parent irq\n", node);
		ret = -EINVAL;
		goto remove_domain;
	}

	irq_set_chained_handler_and_data(csr->parent_irq,
					 xilinx_smmu_csr_irq_handler, csr);

	pr_debug("%pOF: Xilinx SMMU CSR interrupt controller registered\n", node);

	return 0;

remove_domain:
	irq_domain_remove(csr->domain);
unmap:
	iounmap(csr->base);
free:
	kfree(csr);
	return ret;
}

IRQCHIP_DECLARE(xilinx_smmu_csr, "xlnx,versal-net-smmu-csr",
		xilinx_smmu_csr_init);
