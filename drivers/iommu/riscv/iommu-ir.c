// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU Interrupt Remapping
 *
 * Copyright (c) 2026 Qualcomm Technologies, Inc.
 */
#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/irqchip/riscv-imsic.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "iommu.h"

/*
 * Compute the MSI index for an MSI physical address using the
 * IOMMU "extract" function (RISC-V IOMMU spec section 2.3.3).
 */
static size_t riscv_iommu_ir_extract_msi_idx(phys_addr_t pa)
{
	const struct imsic_global_config *global = imsic_get_global_config();
	phys_addr_t mask, addr = pa >> 12;
	size_t idx;

	mask = BIT(global->hart_index_bits + global->guest_index_bits) - 1;
	idx = addr & mask;

	if (global->group_index_bits) {
		phys_addr_t group_mask = BIT(global->group_index_bits) - 1;
		phys_addr_t group_shift = global->group_index_shift - 12;
		phys_addr_t group = (addr >> group_shift) & group_mask;

		idx |= group << fls64(mask);
	}

	return idx;
}

static size_t riscv_iommu_ir_msi_iova_idx(phys_addr_t pa)
{
	const struct imsic_global_config *global = imsic_get_global_config();

	/* msi_iova[] is only used for the host imsics */
	return riscv_iommu_ir_extract_msi_idx(pa) >> global->guest_index_bits;
}

static size_t riscv_iommu_ir_msi_iova_count(void)
{
	const struct imsic_global_config *global = imsic_get_global_config();

	return BIT(global->group_index_bits + global->hart_index_bits);
}

static int riscv_iommu_ir_build_msi_iova(struct riscv_iommu_domain *domain, struct device *dev)
{
	const struct imsic_global_config *global = imsic_get_global_config();
	struct iommu_domain *d = &domain->domain;
	dma_addr_t *msi_iova;
	unsigned int cpu;
	int ret;

	guard(mutex)(&domain->mutex);

	if (domain->msi_iova)
		return 0;

	switch (d->cookie_type) {
	case IOMMU_COOKIE_DMA_IOVA:
	case IOMMU_COOKIE_DMA_MSI:
	case IOMMU_COOKIE_IOMMUFD:
		break;
	default:
		return 0;
	}

	msi_iova = vcalloc(riscv_iommu_ir_msi_iova_count(), sizeof(*msi_iova));
	if (!msi_iova)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		const struct imsic_local_config *local = per_cpu_ptr(global->local, cpu);
		phys_addr_t pa = local->msi_pa;
		unsigned int shift;
		size_t idx;

		if (!pa)
			continue;

		idx = riscv_iommu_ir_msi_iova_idx(pa);
		ret = iommu_dma_map_msi(d, dev, pa, IMSIC_MMIO_PAGE_SZ, &msi_iova[idx], &shift);
		if (ret)
			goto err_free;
		if (shift != IMSIC_MMIO_PAGE_SHIFT) {
			ret = -EBUSY;
			goto err_free;
		}
	}

	domain->msi_iova = msi_iova;

	return 0;

err_free:
	vfree(msi_iova);
	return ret;
}

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
	struct riscv_iommu_info *info = irqdomain->host_data;
	int ret;

	/*
	 * MSI IOVAs are domain-local, just like DMA IOVAs. The device must be
	 * quiesced, including MSI teardown, before switching away from or freeing
	 * the domain. iommu_dma_map_msi() requires the group mutex to be held;
	 * take it around the domain lookup too so info->domain can't change
	 * out from under the build. Bump info->nr_irqs here too, before
	 * irq_domain_alloc_irqs_parent() runs unlocked below, so a concurrent
	 * riscv_iommu_ir_attach_paging_domain() can never observe a count that
	 * is lower than the number of IRQs actually in flight for this device.
	 */
	scoped_guard(iommu_group, info->dev) {
		struct riscv_iommu_domain *domain = rcu_dereference_protected(info->domain, true);

		ret = domain ? riscv_iommu_ir_build_msi_iova(domain, info->dev) : 0;
		if (!ret)
			info->nr_irqs += nr_irqs;
	}
	if (ret)
		return ret;

	ret = irq_domain_alloc_irqs_parent(irqdomain, irq_base, nr_irqs, arg);
	if (ret) {
		guard(iommu_group)(info->dev);
		info->nr_irqs -= nr_irqs;
		return ret;
	}

	for (unsigned int i = 0; i < nr_irqs; i++) {
		struct irq_data *data = irq_domain_get_irq_data(irqdomain, irq_base + i);

		data->chip = &riscv_iommu_ir_irq_chip;
	}

	return 0;
}

static void riscv_iommu_ir_irq_domain_free_irqs(struct irq_domain *irqdomain,
						unsigned int irq_base, unsigned int nr_irqs)
{
	struct riscv_iommu_info *info = irqdomain->host_data;

	irq_domain_free_irqs_parent(irqdomain, irq_base, nr_irqs);

	/*
	 * Decrement only after the parent free completes, so a concurrent
	 * riscv_iommu_ir_attach_paging_domain() never observes a count lower
	 * than the number of IRQs that are actually still live.
	 */
	scoped_guard(iommu_group, info->dev)
		info->nr_irqs -= nr_irqs;
}

static const struct irq_domain_ops riscv_iommu_ir_irq_domain_ops = {
	.alloc			= riscv_iommu_ir_irq_domain_alloc_irqs,
	.free			= riscv_iommu_ir_irq_domain_free_irqs,
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

static void riscv_iommu_ir_refresh_msi_domain(struct device *dev)
{
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct irq_domain *irqdomain;

	if (dev_get_msi_domain(dev) || !dev_is_platform(dev))
		return;

	if (is_of_node(fwnode)) {
		of_msi_configure(dev, to_of_node(fwnode));
	} else if (is_acpi_device_node(fwnode)) {
		fwnode = imsic_acpi_get_fwnode(dev);
		irqdomain = irq_find_matching_fwnode(fwnode, DOMAIN_BUS_PLATFORM_MSI);
		if (irqdomain)
			dev_set_msi_domain(dev, irqdomain);
	}
}

struct irq_domain *riscv_iommu_ir_irq_domain_create(struct device *dev,
						    struct riscv_iommu_info *info)
{
	struct irq_domain *irqparent;
	char *fwname __free(kfree) = NULL;
	struct irq_domain *irqdomain;
	struct fwnode_handle *fn;

	riscv_iommu_ir_refresh_msi_domain(dev);
	irqparent = dev_get_msi_domain(dev);

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

	/*
	 * Publication is deferred to riscv_iommu_ir_irq_domain_publish(),
	 * called from probe_finalize() after the IOMMU core assigns
	 * dev->iommu_group, because the allocation callback locks the group.
	 */
	return irqdomain;
}

void riscv_iommu_ir_irq_domain_publish(struct device *dev)
{
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);

	if (info->irqdomain)
		dev_set_msi_domain(dev, info->irqdomain);
}

void riscv_iommu_ir_irq_domain_remove(struct device *dev, struct riscv_iommu_info *info)
{
	struct fwnode_handle *fn;

	if (!info->irqdomain)
		return;

	if (dev_get_msi_domain(dev) == info->irqdomain)
		dev_set_msi_domain(dev, info->irqdomain->parent);
	fn = info->irqdomain->fwnode;
	irq_domain_remove(info->irqdomain);
	info->irqdomain = NULL;
	irq_domain_free_fwnode(fn);
}

int riscv_iommu_ir_attach_paging_domain(struct iommu_domain *iommu_domain, struct device *dev,
					struct iommu_domain *old)
{
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);

	/*
	 * Build the table if this device has allocated MSIs, since those MSIs may
	 * already be live and expecting riscv_iommu_ir_compose_msi_msg() to find
	 * a populated table for whatever domain is now attached.
	 */
	if (info->nr_irqs)
		return riscv_iommu_ir_build_msi_iova(domain, dev);

	return 0;
}

void riscv_iommu_ir_free_paging_domain(struct iommu_domain *iommu_domain)
{
	struct riscv_iommu_domain *domain = iommu_domain_to_riscv(iommu_domain);

	vfree(domain->msi_iova);
	domain->msi_iova = NULL;
}

void riscv_iommu_ir_get_resv_regions(struct device *dev, struct list_head *head)
{
	struct riscv_iommu_info *info = dev_iommu_priv_get(dev);
	struct iommu_resv_region *region;

	if (!info || !info->irqdomain)
		return;

	region = iommu_alloc_resv_region(RISCV_IOMMU_MSI_IOVA_BASE,
					 (size_t)num_possible_cpus() * PAGE_SIZE,
					 IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO,
					 IOMMU_RESV_SW_MSI, GFP_KERNEL);
	if (region)
		list_add_tail(&region->list, head);
}
