// SPDX-License-Identifier: GPL-2.0
/*
 * Based on arch/x86/kernel/apic/msi.c and kernel/irq/msi.c
 * Copyright (c) 2019 Intel Corporation.
 * Copyright (c) 2020-2022, MaxLinear, Inc.
 * Copyright (c) 2026 TDT AG.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/platform_device.h>

#include <asm/irqdomain.h>
#include <asm/apic.h>

#define MSI_MSGA(x)	((x) << 2)
#define MSI_MSGD(x)	(0x200 + ((x) << 2))
#define MSI_CTRL	0x400
#define MSI_CTRL_EN	BIT(0)
#define MSI_MSK_L	0x404
#define MSI_MSK_H	0x408

#define NMI_MSI_47	47
#define NMI_MSI_49	49
#define NMI_MSI_62	62
#define NMI_MSI_63	63

#define MAX_SOC_MSI_IRQ_PINS	64

struct soc_msi_dev {
	struct device		*dev;
	void __iomem		*base;
	raw_spinlock_t		lock; /* protect register handling */
};

struct soc_nmi_msi {
	irq_hw_number_t irq;
	int		cpuid;
};

static const struct soc_nmi_msi nmi_msi[] = {
	{NMI_MSI_47, 3},
	{NMI_MSI_49, 2},
	{NMI_MSI_62, 1},
	{NMI_MSI_63, 0},
	{ },
};

static bool soc_nmi_msi(irq_hw_number_t hwirq)
{
	if (hwirq == NMI_MSI_47 || hwirq == NMI_MSI_49 ||
	    hwirq == NMI_MSI_62 || hwirq == NMI_MSI_63)
		return true;

	return false;
}

static u32 nmi_irq_to_cpuid(irq_hw_number_t hwirq)
{
	int i;
	unsigned int nr_pcpus = num_possible_cpus();

	for (i = 0; i < ARRAY_SIZE(nmi_msi); i++) {
		if (nmi_msi[i].irq == hwirq) {
			if (nmi_msi[i].cpuid >= nr_pcpus) {
				WARN(1, "NMI on invalid CPU: cpu: %d\n",
				     nmi_msi[i].cpuid);
				return -EINVAL;
			}
			return nmi_msi[i].cpuid;
		}
	}

	WARN((i >= ARRAY_SIZE(nmi_msi)), "Should never come");

	return -EINVAL;
}

static inline void
soc_msi_update_bits(struct soc_msi_dev *mdev, u32 clr, u32 set, u32 ofs)
{
	writel((readl(mdev->base +  ofs) & ~clr) | set, mdev->base + ofs);
}

static void soc_msi_enable(struct soc_msi_dev *mdev)
{
	soc_msi_update_bits(mdev, MSI_CTRL_EN, MSI_CTRL_EN, MSI_CTRL);
}

static void soc_msi_write_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct soc_msi_dev *mdev = irq_data_get_irq_chip_data(d);
	unsigned long flag;

	raw_spin_lock_irqsave(&mdev->lock, flag);
	writel(msg->address_lo, mdev->base + MSI_MSGA(d->hwirq));
	writel(msg->data, mdev->base + MSI_MSGD(d->hwirq));
	raw_spin_unlock_irqrestore(&mdev->lock, flag);
}

static void soc_msi_mask(struct irq_data *d)
{
	struct soc_msi_dev *mdev = irq_data_get_irq_chip_data(d);
	unsigned long flag;
	u32 off = d->hwirq < 32 ? MSI_MSK_L : MSI_MSK_H;

	raw_spin_lock_irqsave(&mdev->lock, flag);
	soc_msi_update_bits(mdev, 0, BIT(d->hwirq % 32), off);
	raw_spin_unlock_irqrestore(&mdev->lock, flag);
}

static void soc_msi_unmask(struct irq_data *d)
{
	struct soc_msi_dev *mdev = irq_data_get_irq_chip_data(d);
	unsigned long flag;
	u32 off = d->hwirq < 32 ? MSI_MSK_L : MSI_MSK_H;

	raw_spin_lock_irqsave(&mdev->lock, flag);
	soc_msi_update_bits(mdev, BIT(d->hwirq % 32), 0, off);
	raw_spin_unlock_irqrestore(&mdev->lock, flag);
}

static void nmi_msi_compose_msg(struct soc_msi_dev *mdev, irq_hw_number_t hwirq)
{
	struct msi_msg msg = {0};
	unsigned long flag;
	u32 cpuid, destid;
	u32 off;

	cpuid = nmi_irq_to_cpuid(hwirq);
	if (cpuid < 0)
		return;

	destid = apic->calc_dest_apicid(cpuid);
	off = hwirq < 32 ? MSI_MSK_L : MSI_MSK_H;

	msg.arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;
	msg.arch_addr_lo.dest_mode_logical = apic->dest_mode_logical;
	msg.arch_addr_lo.redirect_hint = 0;
	msg.arch_addr_lo.destid_0_7 = destid & 0xFF;

	msg.address_hi = X86_MSI_BASE_ADDRESS_HIGH;

	/*
	 * On edge trigger, we don't care about assert level. Also,
	 * since delivery mode is NMI, no irq vector is needed.
	 */
	msg.arch_data.is_level = 0;
	msg.arch_data.delivery_mode = APIC_DELIVERY_MODE_NMI;

	raw_spin_lock_irqsave(&mdev->lock, flag);
	writel(msg.address_lo, mdev->base + MSI_MSGA(hwirq));
	writel(msg.data, mdev->base + MSI_MSGD(hwirq));
	soc_msi_update_bits(mdev, BIT(hwirq % 32), 0, off);
	raw_spin_unlock_irqrestore(&mdev->lock, flag);
}

static void soc_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	__irq_msi_compose_msg(irqd_cfg(data), msg, false);
}

static struct irq_chip soc_msi_irq_chip = {
	.name			= "SOC-MSI",
	.irq_unmask		= soc_msi_unmask,
	.irq_mask		= soc_msi_mask,
	.irq_ack		= irq_chip_ack_parent,
	.irq_set_affinity	= msi_domain_set_affinity,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_compose_msi_msg	= soc_msi_compose_msg,
	.irq_write_msi_msg	= soc_msi_write_msg,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static int soc_msi_domain_xlate(struct irq_domain *d, struct irq_fwspec *fwspec,
				unsigned long *hwirq, unsigned int *type)
{
	if (fwspec->param_count != 1)
		return -EINVAL;

	if (fwspec->param[0] > MAX_SOC_MSI_IRQ_PINS - 1)
		return -EINVAL;

	*hwirq = fwspec->param[0];

	return 0;
}

static int soc_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	struct msi_domain_info *msi_info = domain->host_data;
	struct irq_fwspec *fwspec = arg;
	struct irq_alloc_info info;
	irq_hw_number_t hwirq;
	unsigned int type;
	void *chip_data;
	int i, ret;

	if (!msi_info)
		return -EINVAL;

	chip_data = msi_info->chip_data;

	ret = soc_msi_domain_xlate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	if (irq_find_mapping(domain, hwirq) > 0)
		return -EEXIST;

	/*
	 * All NMI interrupts go to vector 2, no irq mapping needed.
	 * What we want is to configure hardware once, don't do anything else.
	 * 0 means it will continue to initialize other stuff in the irqdomain.
	 * We can just return other value after hw initialized. In this case,
	 * irqdomain will release all resources.
	 */
	if (soc_nmi_msi(hwirq)) {
		nmi_msi_compose_msg((struct soc_msi_dev *)chip_data, hwirq);
		return -EINVAL;
	}

	/* Translate to X86 favorite arguments */
	init_irq_alloc_info(&info, NULL);

	/* Need to allocate from x86 vector domain */
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &info);
	if (ret < 0)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, hwirq + i,
				    &soc_msi_irq_chip, chip_data,
				    handle_edge_irq, chip_data, "edge");
	}

	return 0;
}

static int soc_msi_domain_activate(struct irq_domain *domain,
				   struct irq_data *irq_data, bool early)
{
	struct msi_msg msg[2] = { [1] = { }, };

	WARN_ON(irq_chip_compose_msi_msg(irq_data, msg));
	soc_msi_write_msg(irq_data, msg);

	return 0;
}

static void soc_msi_domain_deactivate(struct irq_domain *domain,
				      struct irq_data *irq_data)
{
	struct msi_msg msg[2];

	memset(msg, 0, sizeof(msg));
	soc_msi_write_msg(irq_data, msg);
}

static const struct irq_domain_ops soc_msi_domain_ops = {
	.translate	= soc_msi_domain_xlate,
	.alloc		= soc_msi_domain_alloc,
	.free		= irq_domain_free_irqs_common,
	.activate	= soc_msi_domain_activate,
	.deactivate	= soc_msi_domain_deactivate,
};

static int intel_soc_msi_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct msi_domain_info *msi_info;
	struct irq_domain *domain;
	struct soc_msi_dev *mdev;
	struct resource *res;

	mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;

	msi_info = devm_kzalloc(&pdev->dev, sizeof(*msi_info), GFP_KERNEL);
	if (!msi_info)
		return -ENOMEM;

	mdev->dev = &pdev->dev;

	msi_info->flags = MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS;
	msi_info->chip_data = mdev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	mdev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mdev->base)) {
		dev_err(&pdev->dev, "failed to ioremap %pR\n", res);
		return PTR_ERR(mdev->base);
	}

	domain = irq_domain_create_hierarchy(x86_vector_domain, 0,
					     MAX_SOC_MSI_IRQ_PINS,
					     of_fwnode_handle(node),
					     &soc_msi_domain_ops, msi_info);

	if (!domain) {
		dev_err(&pdev->dev, "Failed to create soc_msi@%p irqdomain",
			mdev->base);
		return -ENOMEM;
	}

	raw_spin_lock_init(&mdev->lock);

	platform_set_drvdata(pdev, mdev);

	soc_msi_enable(mdev);

	dev_info(&pdev->dev, "Intel SoC MSI init done\n");

	return 0;
}

static const struct of_device_id of_intel_soc_msi_match[] = {
	{ .compatible = "intel,soc-msi" },
	{}
};

static struct platform_driver intel_soc_msi_driver = {
	.probe			= intel_soc_msi_probe,
	.driver = {
		.name		= "intel soc-msi",
		.of_match_table	= of_intel_soc_msi_match,
	},
};
builtin_platform_driver(intel_soc_msi_driver);
