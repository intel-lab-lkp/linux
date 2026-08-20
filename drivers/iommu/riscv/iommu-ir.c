// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU Interrupt Remapping
 *
 * Copyright (c) 2026 Qualcomm Technologies, Inc.
 */
#include <linux/cleanup.h>
#include <linux/msi.h>
#include <linux/slab.h>

#include "iommu.h"

static struct irq_chip riscv_iommu_ir_irq_chip = {
	.name			= "IOMMU-IR",
	.irq_ack		= irq_chip_ack_parent,
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
};

static int riscv_iommu_ir_irq_domain_alloc_irqs(struct irq_domain *irqdomain,
						unsigned int irq_base, unsigned int nr_irqs,
						void *arg)
{
	int ret;

	ret = irq_domain_alloc_irqs_parent(irqdomain, irq_base, nr_irqs, arg);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < nr_irqs; i++) {
		struct irq_data *data = irq_domain_get_irq_data(irqdomain, irq_base + i);

		data->chip = &riscv_iommu_ir_irq_chip;
	}

	return 0;
}

static const struct irq_domain_ops riscv_iommu_ir_irq_domain_ops = {
	.alloc			= riscv_iommu_ir_irq_domain_alloc_irqs,
	.free			= irq_domain_free_irqs_parent,
};

static const struct msi_parent_ops riscv_iommu_ir_msi_parent_ops = {
	.prefix			= "IR-",
	.supported_flags	= MSI_GENERIC_FLAGS_MASK |
				  MSI_FLAG_PCI_MSIX,
	.required_flags		= MSI_FLAG_USE_DEF_DOM_OPS |
				  MSI_FLAG_USE_DEF_CHIP_OPS |
				  MSI_FLAG_PCI_MSI_MASK_PARENT,
	.chip_flags		= MSI_CHIP_FLAG_SET_ACK,
	.init_dev_msi_info	= msi_parent_init_dev_msi_info,
};

struct irq_domain *riscv_iommu_ir_irq_domain_create(struct device *dev,
						    struct riscv_iommu_info *info)
{
	struct irq_domain *irqparent = dev_get_msi_domain(dev);
	char *fwname __free(kfree) = NULL;
	struct irq_domain *irqdomain;
	struct fwnode_handle *fn;

	if (!irqparent)
		return NULL;

	fwname = kasprintf(GFP_KERNEL, "IOMMU-IR-%s", dev_name(dev));
	if (!fwname)
		return ERR_PTR(-ENOMEM);

	fn = irq_domain_alloc_named_fwnode(fwname);
	if (!fn)
		return ERR_PTR(-ENOMEM);

	irqdomain = irq_domain_create_hierarchy(irqparent, 0, 0, fn,
						&riscv_iommu_ir_irq_domain_ops, info);
	if (!irqdomain) {
		irq_domain_free_fwnode(fn);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * The RISC-V IOMMU doesn't validate MSI data, so we can't set
	 * IRQ_DOMAIN_FLAG_ISOLATED_MSI. This means VFIO requires its
	 * allow_unsafe_interrupts module parameter.
	 */
	irqdomain->flags |= IRQ_DOMAIN_FLAG_MSI_PARENT;
	irqdomain->msi_parent_ops = &riscv_iommu_ir_msi_parent_ops;
	irq_domain_update_bus_token(irqdomain, DOMAIN_BUS_MSI_REMAP);

	dev_set_msi_domain(dev, irqdomain);

	return irqdomain;
}

void riscv_iommu_ir_irq_domain_remove(struct device *dev, struct riscv_iommu_info *info)
{
	struct fwnode_handle *fn;

	if (!info->irqdomain)
		return;

	dev_set_msi_domain(dev, info->irqdomain->parent);
	fn = info->irqdomain->fwnode;
	irq_domain_remove(info->irqdomain);
	info->irqdomain = NULL;
	irq_domain_free_fwnode(fn);
}

int riscv_iommu_ir_attach_paging_domain(struct iommu_domain *iommu_domain, struct device *dev,
					struct iommu_domain *old)
{
	return 0;
}

void riscv_iommu_ir_free_paging_domain(struct iommu_domain *iommu_domain)
{
}
