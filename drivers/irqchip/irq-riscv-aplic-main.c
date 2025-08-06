// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 Western Digital Corporation or its affiliates.
 * Copyright (C) 2022 Ventana Micro Systems Inc.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/irqchip/riscv-aplic.h>
#include <linux/irqchip/riscv-imsic.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/printk.h>
#include <linux/syscore_ops.h>

#include "irq-riscv-aplic-main.h"

static LIST_HEAD(aplics);

static void aplic_restore(struct aplic_priv *priv)
{
	int i;
	u32 clrip;

	writel(priv->saved_domaincfg, priv->regs + APLIC_DOMAINCFG);
#ifdef CONFIG_RISCV_M_MODE
	writel(priv->saved_msiaddr, priv->regs + APLIC_xMSICFGADDR);
	writel(priv->saved_msiaddrh, priv->regs + APLIC_xMSICFGADDRH);
#endif
	for (i = 1; i <= priv->nr_irqs; i++) {
		writel(priv->saved_sourcecfg[i - 1],
		       priv->regs + APLIC_SOURCECFG_BASE +
		       (i - 1) * sizeof(u32));
		writel(priv->saved_target[i - 1],
		       priv->regs + APLIC_TARGET_BASE +
		       (i - 1) * sizeof(u32));
	}

	for (i = 0; i <= priv->nr_irqs; i += 32) {
		writel(-1U, priv->regs + APLIC_CLRIE_BASE +
			    (i / 32) * sizeof(u32));
		writel(priv->saved_ie[i / 32],
		       priv->regs + APLIC_SETIE_BASE +
		       (i / 32) * sizeof(u32));
	}

	if (priv->nr_idcs) {
		aplic_direct_restore(priv);
	} else {
		/* Re-trigger the interrupts */
		for (i = 0; i <= priv->nr_irqs; i += 32) {
			clrip = readl(priv->regs + APLIC_CLRIP_BASE +
				      (i / 32) * sizeof(u32));
			writel(clrip, priv->regs + APLIC_SETIP_BASE +
				      (i / 32) * sizeof(u32));
		}
	}
}

static void aplic_save(struct aplic_priv *priv)
{
	int i;

	for (i = 1; i <= priv->nr_irqs; i++) {
		priv->saved_target[i - 1] = readl(priv->regs +
						  APLIC_TARGET_BASE +
						  (i - 1) * sizeof(u32));
	}

	for (i = 0; i <= priv->nr_irqs; i += 32) {
		priv->saved_ie[i / 32] = readl(priv->regs +
					       APLIC_SETIE_BASE +
					       (i / 32) * sizeof(u32));
	}
}

static int aplic_syscore_suspend(void)
{
	struct aplic_priv *priv;

	list_for_each_entry(priv, &aplics, head) {
		aplic_save(priv);
	}

	return 0;
}

static void aplic_syscore_resume(void)
{
	struct aplic_priv *priv;

	list_for_each_entry(priv, &aplics, head) {
		aplic_restore(priv);
	}
}

static struct syscore_ops aplic_syscore_ops = {
	.suspend = aplic_syscore_suspend,
	.resume = aplic_syscore_resume,
};

static int aplic_pm_notifier(struct notifier_block *nb, unsigned long action, void *data)
{
	struct aplic_priv *priv = container_of(nb, struct aplic_priv, genpd_nb);

	switch (action) {
	case GENPD_NOTIFY_PRE_OFF:
		aplic_save(priv);
		break;
	case GENPD_NOTIFY_ON:
		aplic_restore(priv);
		break;
	default:
		break;
	}

	return 0;
}

static void aplic_remove(void *data)
{
	struct aplic_priv *priv = data;

	list_del(&priv->head);
	dev_pm_genpd_remove_notifier(priv->dev);
}

static int aplic_add(struct device *dev, struct aplic_priv *priv)
{
	int ret;

	list_add(&priv->head, &aplics);
	/* Add genpd notifier */
	priv->genpd_nb.notifier_call = aplic_pm_notifier;
	ret = dev_pm_genpd_add_notifier(dev, &priv->genpd_nb);
	if (ret && ret != -ENODEV && ret != -EOPNOTSUPP) {
		list_del(&priv->head);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, aplic_remove, priv);
	if (ret)
		return ret;

	return devm_pm_runtime_enable(dev);
}

void aplic_irq_unmask(struct irq_data *d)
{
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(d->hwirq, priv->regs + APLIC_SETIENUM);
}

void aplic_irq_mask(struct irq_data *d)
{
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);

	writel(d->hwirq, priv->regs + APLIC_CLRIENUM);
}

int aplic_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct aplic_priv *priv = irq_data_get_irq_chip_data(d);
	void __iomem *sourcecfg;
	u32 val = 0;

	switch (type) {
	case IRQ_TYPE_NONE:
		val = APLIC_SOURCECFG_SM_INACTIVE;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		val = APLIC_SOURCECFG_SM_LEVEL_LOW;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		val = APLIC_SOURCECFG_SM_LEVEL_HIGH;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		val = APLIC_SOURCECFG_SM_EDGE_FALL;
		break;
	case IRQ_TYPE_EDGE_RISING:
		val = APLIC_SOURCECFG_SM_EDGE_RISE;
		break;
	default:
		return -EINVAL;
	}

	sourcecfg = priv->regs + APLIC_SOURCECFG_BASE;
	sourcecfg += (d->hwirq - 1) * sizeof(u32);
	writel(val, sourcecfg);
	priv->saved_sourcecfg[d->hwirq - 1] = val;

	return 0;
}

int aplic_irqdomain_translate(struct irq_fwspec *fwspec, u32 gsi_base,
			      unsigned long *hwirq, unsigned int *type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	if (WARN_ON(!fwspec->param[0]))
		return -EINVAL;

	/* For DT, gsi_base is always zero. */
	*hwirq = fwspec->param[0] - gsi_base;
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;

	WARN_ON(*type == IRQ_TYPE_NONE);

	return 0;
}

void aplic_init_hw_global(struct aplic_priv *priv, bool msi_mode)
{
	u32 val;
#ifdef CONFIG_RISCV_M_MODE
	u32 valh;

	if (msi_mode) {
		val = lower_32_bits(priv->msicfg.base_ppn);
		valh = FIELD_PREP(APLIC_xMSICFGADDRH_BAPPN, upper_32_bits(priv->msicfg.base_ppn));
		valh |= FIELD_PREP(APLIC_xMSICFGADDRH_LHXW, priv->msicfg.lhxw);
		valh |= FIELD_PREP(APLIC_xMSICFGADDRH_HHXW, priv->msicfg.hhxw);
		valh |= FIELD_PREP(APLIC_xMSICFGADDRH_LHXS, priv->msicfg.lhxs);
		valh |= FIELD_PREP(APLIC_xMSICFGADDRH_HHXS, priv->msicfg.hhxs);
		writel(val, priv->regs + APLIC_xMSICFGADDR);
		writel(valh, priv->regs + APLIC_xMSICFGADDRH);
		priv->saved_msiaddr = val;
		priv->saved_msiaddrh = valh;
	}
#endif

	/* Setup APLIC domaincfg register */
	val = readl(priv->regs + APLIC_DOMAINCFG);
	val |= APLIC_DOMAINCFG_IE;
	if (msi_mode)
		val |= APLIC_DOMAINCFG_DM;
	writel(val, priv->regs + APLIC_DOMAINCFG);
	if (readl(priv->regs + APLIC_DOMAINCFG) != val)
		dev_warn(priv->dev, "unable to write 0x%x in domaincfg\n", val);
	priv->saved_domaincfg = val;
}

static void aplic_init_hw_irqs(struct aplic_priv *priv)
{
	int i;

	/* Disable all interrupts */
	for (i = 0; i <= priv->nr_irqs; i += 32)
		writel(-1U, priv->regs + APLIC_CLRIE_BASE + (i / 32) * sizeof(u32));

	/* Set interrupt type and default priority for all interrupts */
	for (i = 1; i <= priv->nr_irqs; i++) {
		writel(0, priv->regs + APLIC_SOURCECFG_BASE + (i - 1) * sizeof(u32));
		writel(APLIC_DEFAULT_PRIORITY,
		       priv->regs + APLIC_TARGET_BASE + (i - 1) * sizeof(u32));
	}

	/* Clear APLIC domaincfg */
	writel(0, priv->regs + APLIC_DOMAINCFG);
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id aplic_acpi_match[] = {
	{ "RSCV0002", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, aplic_acpi_match);

#endif

int aplic_setup_priv(struct aplic_priv *priv, struct device *dev, void __iomem *regs)
{
	struct device_node *np = to_of_node(dev->fwnode);
	struct of_phandle_args parent;
	int rc;

	/* Save device pointer and register base */
	priv->dev = dev;
	priv->regs = regs;

	if (np) {
		/* Find out number of interrupt sources */
		rc = of_property_read_u32(np, "riscv,num-sources", &priv->nr_irqs);
		if (rc) {
			dev_err(dev, "failed to get number of interrupt sources\n");
			return rc;
		}

		/*
		 * Find out number of IDCs based on parent interrupts
		 *
		 * If "msi-parent" property is present then we ignore the
		 * APLIC IDCs which forces the APLIC driver to use MSI mode.
		 */
		if (!of_property_present(np, "msi-parent")) {
			while (!of_irq_parse_one(np, priv->nr_idcs, &parent))
				priv->nr_idcs++;
		}
	} else {
		rc = riscv_acpi_get_gsi_info(dev->fwnode, &priv->gsi_base, &priv->acpi_aplic_id,
					     &priv->nr_irqs, &priv->nr_idcs);
		if (rc) {
			dev_err(dev, "failed to find GSI mapping\n");
			return rc;
		}
	}

	/* Setup initial state APLIC interrupts */
	aplic_init_hw_irqs(priv);

	/* For power management */
	priv->saved_target = devm_kzalloc(dev, priv->nr_irqs * sizeof(u32),
					  GFP_KERNEL);
	if (!priv->saved_target)
		return -ENOMEM;

	priv->saved_sourcecfg = devm_kzalloc(dev, priv->nr_irqs * sizeof(u32),
					     GFP_KERNEL);
	if (!priv->saved_sourcecfg)
		return -ENOMEM;

	priv->saved_ie = devm_kzalloc(dev,
				      DIV_ROUND_UP(priv->nr_irqs, 32) * sizeof(u32),
				      GFP_KERNEL);
	if (!priv->saved_ie)
		return -ENOMEM;

	return aplic_add(dev, priv);
}

static int aplic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	bool msi_mode = false;
	void __iomem *regs;
	int rc;

	/* Map the MMIO registers */
	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs)) {
		dev_err(dev, "failed map MMIO registers\n");
		return PTR_ERR(regs);
	}

	/*
	 * If msi-parent property is present then setup APLIC MSI
	 * mode otherwise setup APLIC direct mode.
	 */
	if (is_of_node(dev->fwnode))
		msi_mode = of_property_present(to_of_node(dev->fwnode), "msi-parent");
	else
		msi_mode = imsic_acpi_get_fwnode(NULL) ? 1 : 0;

	if (msi_mode)
		rc = aplic_msi_setup(dev, regs);
	else
		rc = aplic_direct_setup(dev, regs);
	if (rc)
		dev_err_probe(dev, rc, "failed to setup APLIC in %s mode\n",
			      msi_mode ? "MSI" : "direct");
	else
		register_syscore_ops(&aplic_syscore_ops);

#ifdef CONFIG_ACPI
	if (!acpi_disabled)
		acpi_dev_clear_dependencies(ACPI_COMPANION(dev));
#endif

	return rc;
}

static const struct of_device_id aplic_match[] = {
	{ .compatible = "riscv,aplic" },
	{}
};

static struct platform_driver aplic_driver = {
	.driver = {
		.name		= "riscv-aplic",
		.of_match_table	= aplic_match,
		.acpi_match_table = ACPI_PTR(aplic_acpi_match),
	},
	.probe = aplic_probe,
};
builtin_platform_driver(aplic_driver);
