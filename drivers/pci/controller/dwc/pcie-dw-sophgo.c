// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Sophgo SoCs.
 *
 */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/property.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "pcie-designware.h"

#define to_sophgo_pcie(x)		dev_get_drvdata((x)->dev)

#define PCIE_INT_SIGNAL			0xc48
#define PCIE_INT_EN			0xca0

#define PCIE_SIGNAL_INTX_SHIFT		5

#define PCIE_INT_EN_INTX_SHIFT		1
#define PCIE_INT_EN_INT_SII		BIT(0)
#define PCIE_INT_EN_INT_INTA		BIT(1)
#define PCIE_INT_EN_INT_INTB		BIT(2)
#define PCIE_INT_EN_INT_INTC		BIT(3)
#define PCIE_INT_EN_INT_INTD		BIT(4)
#define PCIE_INT_EN_INT_MSI		BIT(5)

struct sophgo_pcie {
	struct dw_pcie pci;
	void __iomem *app_base;
	struct clk_bulk_data *clks;
	unsigned int clk_cnt;
	struct reset_control *rst;
	struct irq_domain *irq_domain;
};

static int sophgo_pcie_readl_app(struct sophgo_pcie *sophgo, u32 reg)
{
	return readl_relaxed(sophgo->app_base + reg);
}

static void sophgo_pcie_writel_app(struct sophgo_pcie *sophgo, u32 val, u32 reg)
{
	writel_relaxed(val, sophgo->app_base + reg);
}

static void sophgo_pcie_intx_handler(struct irq_desc *desc)
{
	struct dw_pcie_rp *pp = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	unsigned long hwirq = PCIE_SIGNAL_INTX_SHIFT;
	unsigned long reg;

	chained_irq_enter(chip, desc);

	reg = sophgo_pcie_readl_app(sophgo, PCIE_INT_SIGNAL);

	for_each_set_bit_from(hwirq, &reg, PCI_NUM_INTX + PCIE_SIGNAL_INTX_SHIFT)
		generic_handle_domain_irq(sophgo->irq_domain,
					  hwirq - PCIE_SIGNAL_INTX_SHIFT);

	chained_irq_exit(chip, desc);
}

static void sophgo_intx_mask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	val &= ~BIT(d->hwirq + PCIE_INT_EN_INTX_SHIFT);
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
};

static void sophgo_intx_unmask(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	val |= BIT(d->hwirq + PCIE_INT_EN_INTX_SHIFT);
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
};

static void sophgo_intx_eoi(struct irq_data *d)
{
}

static struct irq_chip sophgo_intx_irq_chip = {
	.name			= "INTx",
	.irq_mask		= sophgo_intx_mask,
	.irq_unmask		= sophgo_intx_unmask,
	.irq_eoi		= sophgo_intx_eoi,
};

static int sophgo_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &sophgo_intx_irq_chip, handle_fasteoi_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = sophgo_pcie_intx_map,
};

static int sophgo_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	struct device *dev = sophgo->pci.dev;
	struct fwnode_handle *intc;
	int irq;

	intc = device_get_named_child_node(dev, "interrupt-controller");
	if (!intc) {
		dev_err(dev, "missing child interrupt-controller node\n");
		return -ENODEV;
	}

	irq = fwnode_irq_get(intc, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get INTx irq number\n");
		fwnode_handle_put(intc);
		return irq;
	}

	sophgo->irq_domain = irq_domain_create_linear(intc, PCI_NUM_INTX,
						      &intx_domain_ops, sophgo);
	fwnode_handle_put(intc);
	if (!sophgo->irq_domain) {
		dev_err(dev, "failed to get a INTx irq domain\n");
		return -EINVAL;
	}

	return irq;
}

static void sophgo_pcie_msi_enable(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct sophgo_pcie *sophgo = to_sophgo_pcie(pci);
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pp->lock, flags);

	val = sophgo_pcie_readl_app(sophgo, PCIE_INT_EN);
	val |= PCIE_INT_EN_INT_MSI;
	sophgo_pcie_writel_app(sophgo, val, PCIE_INT_EN);

	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

static int sophgo_pcie_host_init(struct dw_pcie_rp *pp)
{
	int irq;

	irq = sophgo_pcie_init_irq_domain(pp);
	if (irq < 0)
		return irq;

	irq_set_chained_handler_and_data(irq, sophgo_pcie_intx_handler,
					 pp);

	sophgo_pcie_msi_enable(pp);

	return 0;
}

static const struct dw_pcie_host_ops sophgo_pcie_host_ops = {
	.init = sophgo_pcie_host_init,
};

static int sophgo_pcie_clk_init(struct sophgo_pcie *sophgo)
{
	struct device *dev = sophgo->pci.dev;
	int ret;

	ret = devm_clk_bulk_get_all_enabled(dev, &sophgo->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	sophgo->clk_cnt = ret;

	return 0;
}

static int sophgo_pcie_resource_get(struct platform_device *pdev,
				    struct sophgo_pcie *sophgo)
{
	sophgo->app_base = devm_platform_ioremap_resource_byname(pdev, "app");
	if (IS_ERR(sophgo->app_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(sophgo->app_base),
				     "failed to map app registers\n");

	return 0;
}

static int sophgo_pcie_configure_rc(struct sophgo_pcie *sophgo)
{
	struct dw_pcie_rp *pp;

	pp = &sophgo->pci.pp;
	pp->ops = &sophgo_pcie_host_ops;

	return dw_pcie_host_init(pp);
}

static int sophgo_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sophgo_pcie *sophgo;
	int ret;

	sophgo = devm_kzalloc(dev, sizeof(*sophgo), GFP_KERNEL);
	if (!sophgo)
		return -ENOMEM;

	platform_set_drvdata(pdev, sophgo);

	sophgo->pci.dev = dev;

	ret = sophgo_pcie_resource_get(pdev, sophgo);
	if (ret)
		return ret;

	ret = sophgo_pcie_clk_init(sophgo);
	if (ret)
		return ret;

	return sophgo_pcie_configure_rc(sophgo);
}

static const struct of_device_id sophgo_pcie_of_match[] = {
	{ .compatible = "sophgo,sg2044-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, sophgo_pcie_acpi_match);

static const struct acpi_device_id sophgo_pcie_acpi_match[] = {
	{ "SOPHO000", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, sophgo_pcie_acpi_match);

static struct platform_driver sophgo_pcie_driver = {
	.driver = {
		.name = "sophgo-dw-pcie",
		.of_match_table = sophgo_pcie_of_match,
		.acpi_match_table = sophgo_pcie_acpi_match,
		.suppress_bind_attrs = true,
	},
	.probe = sophgo_pcie_probe,
};
builtin_platform_driver(sophgo_pcie_driver);
