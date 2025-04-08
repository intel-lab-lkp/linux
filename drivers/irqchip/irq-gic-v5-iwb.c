// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2025 ARM Limited, All Rights Reserved.
 */
#define pr_fmt(fmt)	"GICv5 IWB: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/iopoll.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "irq-gic-v5.h"

static u32 iwb_readl(struct gicv5_iwb_chip_data *iwb_node, const u64 reg_offset)
{
	return readl_relaxed(iwb_node->iwb_base + reg_offset);
}

static void iwb_writel(struct gicv5_iwb_chip_data *iwb_node, const u32 val,
		       const u64 reg_offset)
{
	writel_relaxed(val, iwb_node->iwb_base + reg_offset);
}

static int gicv5_iwb_wait_for_wenabler(struct gicv5_iwb_chip_data *iwb_node)
{
	int ret;
	u32 val;

	ret = readl_relaxed_poll_timeout_atomic(
			iwb_node->iwb_base + GICV5_IWB_WENABLE_STATUSR, val,
			FIELD_GET(GICV5_IWB_WENABLE_STATUSR_IDLE, val), 1,
			USEC_PER_SEC);

	if (ret == -ETIMEDOUT)
		pr_err_ratelimited("GICv5 IWB_WENABLE_STATUSR timeout\n");

	return ret;
}

static int __gicv5_iwb_set_wire_enable(struct gicv5_iwb_chip_data *iwb_node,
				       u32 iwb_wire, bool enable)
{
	u32 val;
	u32 n = iwb_wire / 32;
	u8 i = iwb_wire % 32;

	if (n >= iwb_node->nr_regs) {
		pr_err("IWB_WENABLER<n> is invalid for n=%u\n", n);
		return -EINVAL;
	}

	/*
	 * Enable IWB wire/pin at this point
	 * Note: This is not the same as enabling the interrupt
	 */
	val = iwb_readl(iwb_node, GICV5_IWB_WENABLER + (4 * n));
	if (enable)
		val |= BIT(i);
	else
		val &= ~BIT(i);
	iwb_writel(iwb_node, val, GICV5_IWB_WENABLER + (4 * n));

	return gicv5_iwb_wait_for_wenabler(iwb_node);
}

static int gicv5_iwb_enable_wire(struct gicv5_iwb_chip_data *iwb_node,
				 u32 iwb_wire)
{
	return __gicv5_iwb_set_wire_enable(iwb_node, iwb_wire, true);
}

static int gicv5_iwb_disable_wire(struct gicv5_iwb_chip_data *iwb_node,
				  u32 iwb_wire)
{
	return __gicv5_iwb_set_wire_enable(iwb_node, iwb_wire, false);
}

static int gicv5_iwb_set_type(struct irq_data *d, unsigned int type)
{
	struct gicv5_iwb_chip_data *iwb_node = irq_data_get_irq_chip_data(d);
	u32 iwb_wire, n, wtmr;
	u8 i;

	iwb_wire = d->hwirq;

	i = iwb_wire % 32;
	n = iwb_wire / 32;

	WARN_ON(n >= iwb_node->nr_regs);

	wtmr = iwb_readl(iwb_node, GICV5_IWB_WTMR + (4 * n));

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_LEVEL_LOW:
		wtmr |= BIT(i);
		break;
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_EDGE_FALLING:
		wtmr &= ~BIT(i);
		break;
	default:
		pr_debug("unexpected wire trigger mode");
		return -EINVAL;
	}

	iwb_writel(iwb_node, wtmr, GICV5_IWB_WTMR + (4 * n));

	return 0;
}

static const struct irq_chip gicv5_iwb_chip = {
	.name			= "GICv5-IWB",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_type		= gicv5_iwb_set_type,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_get_irqchip_state	= irq_chip_get_parent_state,
	.irq_set_irqchip_state	= irq_chip_set_parent_state,
	.flags			= IRQCHIP_SET_TYPE_MASKED |
				  IRQCHIP_SKIP_SET_WAKE |
				  IRQCHIP_MASK_ON_SUSPEND
};

static int gicv5_iwb_irq_domain_translate(struct irq_domain *d,
					  struct irq_fwspec *fwspec,
					  irq_hw_number_t *hwirq,
					  unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count < 2)
			return -EINVAL;

		/*
		 * param[0] is be the wire
		 * param[1] is the interrupt type
		 */
		*hwirq = fwspec->param[0];

		*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;

		return 0;
	}

	return -EINVAL;
}

static void gicv5_iwb_irq_domain_free(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs)
{
	/* Free the local data, and then go up the hierarchy doing the same */
	struct gicv5_iwb_chip_data *iwb_node = domain->host_data;
	struct irq_data *data;

	if (WARN_ON(nr_irqs != 1))
		return;

	data = irq_domain_get_irq_data(domain, virq);
	gicv5_iwb_disable_wire(iwb_node, data->hwirq);

	irq_domain_reset_irq_data(data);

	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}

/*
 * Our parent is the ITS, which expects MSI devices with programmable
 * event IDs. IWB event IDs are hardcoded.
 *
 * Use the msi_alloc_info_t structure to convey both our DeviceID
 * (scratchpad[0]), and the wire that we are attempting to map to an LPI in
 * the ITT (scratchpad[1]).
 */
static int iwb_alloc_lpi_irq_parent(struct irq_domain *domain,
				    unsigned int virq, irq_hw_number_t hwirq)
{
	msi_alloc_info_t info;
	struct gicv5_iwb_chip_data *iwb_node = domain->host_data;

	info.scratchpad[0].ul = iwb_node->device_id;
	info.scratchpad[1].ul = hwirq;
	info.hwirq = hwirq;

	return irq_domain_alloc_irqs_parent(domain, virq, 1, &info);
}

static int gicv5_iwb_irq_domain_alloc(struct irq_domain *domain,
				      unsigned int virq, unsigned int nr_irqs,
				      void *arg)
{
	int ret;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;
	struct irq_data *irqd;
	struct gicv5_iwb_chip_data *iwb_node;

	if (WARN_ON(nr_irqs != 1))
		return -EINVAL;

	ret = gicv5_iwb_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	irqd = irq_desc_get_irq_data(irq_to_desc(virq));
	iwb_node = domain->host_data;

	if (iwb_alloc_lpi_irq_parent(domain, virq, hwirq))
		return -EINVAL;

	gicv5_iwb_enable_wire(iwb_node, hwirq);

	irq_domain_set_info(domain, virq, hwirq, &gicv5_iwb_chip, iwb_node,
			    handle_fasteoi_irq, NULL, NULL);
	irq_set_probe(virq);
	irqd_set_single_target(irqd);

	return 0;
}

static int gicv5_iwb_irq_domain_select(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       enum irq_domain_bus_token bus_token)
{
	struct gicv5_iwb_chip_data *iwb_node;

	/* Not for us */
	if (fwspec->fwnode != d->fwnode)
		return 0;

	iwb_node = d->host_data;

	return (d == iwb_node->domain);
}

static const struct irq_domain_ops gicv5_iwb_irq_domain_ops = {
	.translate	= gicv5_iwb_irq_domain_translate,
	.alloc		= gicv5_iwb_irq_domain_alloc,
	.free		= gicv5_iwb_irq_domain_free,
	.select		= gicv5_iwb_irq_domain_select
};

static struct gicv5_iwb_chip_data *
__init gicv5_iwb_init_bases(void __iomem *iwb_base,
			     struct fwnode_handle *handle,
			     struct irq_domain *parent_domain, u32 device_id)
{
	u32 nr_wires, idr0, cr0;
	struct gicv5_iwb_chip_data *iwb_node;
	struct msi_domain_info *msi_info;
	struct gicv5_its_chip_data *its;
	struct gicv5_its_dev *its_dev;
	int ret;

	msi_info = msi_get_domain_info(parent_domain);
	its = msi_info->data;
	if (!its) {
		pr_warn("IWB %pOF can't find parent ITS, bailing\n",
			to_of_node(handle));
		return ERR_PTR(-ENODEV);
	}

	iwb_node = kzalloc(sizeof(*iwb_node), GFP_KERNEL);
	if (!iwb_node)
		return ERR_PTR(-ENOMEM);

	iwb_node->iwb_base = iwb_base;
	iwb_node->device_id = device_id;

	idr0 = iwb_readl(iwb_node, GICV5_IWB_IDR0);
	nr_wires = (FIELD_GET(GICV5_IWB_IDR0_IW_RANGE, idr0) + 1) * 32;

	iwb_node->domain = irq_domain_create_hierarchy(parent_domain, 0,
			   nr_wires, handle, &gicv5_iwb_irq_domain_ops,
			   iwb_node);
	irq_domain_update_bus_token(iwb_node->domain, DOMAIN_BUS_WIRED);

	cr0 = iwb_readl(iwb_node, GICV5_IWB_CR0);
	if (!FIELD_GET(GICV5_IWB_CR0_IWBEN, cr0)) {
		pr_err("IWB %s must be enabled in firmware\n",
		       fwnode_get_name(handle));
		ret = -EINVAL;
		goto out_free;
	}

	iwb_node->nr_regs = FIELD_GET(GICV5_IWB_IDR0_IW_RANGE, idr0) + 1;

	for (unsigned int n = 0; n < iwb_node->nr_regs; n++)
		iwb_writel(iwb_node, 0, GICV5_IWB_WENABLER + (sizeof(u32) * n));

	ret = gicv5_iwb_wait_for_wenabler(iwb_node);
	if (ret)
		goto out_free;

	guard(mutex)(&its->dev_alloc_lock);

	its_dev = gicv5_its_alloc_device(its, roundup_pow_of_two(nr_wires),
					 device_id, true);
	if (IS_ERR(its_dev)) {
		ret = -ENODEV;
		goto out_free;
	}

	return iwb_node;
out_free:
	irq_domain_remove(iwb_node->domain);
	kfree(iwb_node);

	return ERR_PTR(ret);
}

static int __init gicv5_iwb_of_init(struct device_node *node)
{
	struct gicv5_iwb_chip_data *iwb_node;
	void __iomem *iwb_base;
	u32 device_id;
	struct irq_domain *parent_domain;
	struct device_node *parent_its __free(device_node) = NULL;
	struct of_phandle_args args;
	int ret;

	iwb_base = of_io_request_and_map(node, 0, "IWB");
	if (IS_ERR(iwb_base)) {
		pr_err("%pOF: unable to map GICv5 IWB registers\n", node);
		return PTR_ERR(iwb_base);
	}

	ret = of_parse_phandle_with_args(node, "msi-parent", "#msi-cells", 0,
					 &args);
	if (ret) {
		pr_err("%pOF: Can't retrieve deviceID\n", node);
		goto out_unmap;
	}

	parent_its = args.np;
	parent_domain = irq_find_matching_host(parent_its, DOMAIN_BUS_NEXUS);
	if (!parent_domain) {
		pr_err("Unable to find the parent ITS domain for %pOF!\n", node);
		ret = -ENXIO;
		goto out_unmap;
	}

	device_id = args.args[0];
	pr_debug("IWB deviceID: 0x%x\n", device_id);

	iwb_node = gicv5_iwb_init_bases(iwb_base, &node->fwnode, parent_domain,
					device_id);
	if (IS_ERR(iwb_node)) {
		ret = PTR_ERR(iwb_node);
		goto out_unmap;
	}

	return 0;
out_unmap:
	iounmap(iwb_base);
	return ret;
}

void __init gicv5_iwb_of_probe(struct device_node *parent)
{
	int ret;
	struct device_node *np;

	for_each_available_child_of_node(parent, np) {
		if (!of_device_is_compatible(np, "arm,gic-v5-iwb"))
			continue;

		ret = gicv5_iwb_of_init(np);
		if (ret)
			pr_err("Failed to init IWB %s\n", np->full_name);
	}
}
