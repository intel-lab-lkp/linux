// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for AMD MDB PCIe Bridge
 *
 * Copyright (C) 2024-2025, Advanced Micro Devices, Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define AMD_MDB_TLP_IR_STATUS_MISC		0x4C0
#define AMD_MDB_TLP_IR_MASK_MISC		0x4C4
#define AMD_MDB_TLP_IR_ENABLE_MISC		0x4C8
#define AMD_MDB_TLP_IR_DISABLE_MISC		0x4CC

#define AMD_MDB_TLP_PCIE_INTX_MASK	GENMASK(23, 16)

#define AMD_MDB_PCIE_INTR_INTX_ASSERT(x)	BIT((x) * 2)

/* Interrupt registers definitions */
#define AMD_MDB_PCIE_INTR_CMPL_TIMEOUT		15
#define AMD_MDB_PCIE_INTR_INTX			16
#define AMD_MDB_PCIE_INTR_PM_PME_RCVD		24
#define AMD_MDB_PCIE_INTR_PME_TO_ACK_RCVD	25
#define AMD_MDB_PCIE_INTR_MISC_CORRECTABLE	26
#define AMD_MDB_PCIE_INTR_NONFATAL		27
#define AMD_MDB_PCIE_INTR_FATAL			28

#define IMR(x) BIT(AMD_MDB_PCIE_INTR_ ##x)
#define AMD_MDB_PCIE_IMR_ALL_MASK			\
	(						\
		IMR(CMPL_TIMEOUT)	|		\
		IMR(PM_PME_RCVD)	|		\
		IMR(PME_TO_ACK_RCVD)	|		\
		IMR(MISC_CORRECTABLE)	|		\
		IMR(NONFATAL)		|		\
		IMR(FATAL)		|		\
		AMD_MDB_TLP_PCIE_INTX_MASK		\
	)

#define AMD_MDB_PCIE_INTX_BIT(x) (1U << (2 * (x) + AMD_MDB_PCIE_INTR_INTX))
/**
 * struct amd_mdb_pcie - PCIe port information
 * @pci: DesignWare PCIe controller structure
 * @slcr: MDB System Level Control and Status Register (SLCR) Base
 * @intx_domain: INTx IRQ domain pointer
 * @mdb_domain: MDB IRQ domain pointer
 * @intx_irq: INTx IRQ interrupt number
 */
struct amd_mdb_pcie {
	struct dw_pcie			pci;
	void __iomem			*slcr;
	struct irq_domain		*intx_domain;
	struct irq_domain		*mdb_domain;
	int				intx_irq;
};

static const struct dw_pcie_host_ops amd_mdb_pcie_host_ops = {
};

static inline u32 pcie_read(struct amd_mdb_pcie *pcie, u32 reg)
{
	return readl_relaxed(pcie->slcr + reg);
}

static inline void pcie_write(struct amd_mdb_pcie *pcie,
			      u32 val, u32 reg)
{
	writel_relaxed(val, pcie->slcr + reg);
}

static void amd_mdb_intx_irq_mask(struct irq_data *data)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *port = &pci->pp;
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	val = AMD_MDB_PCIE_INTX_BIT(data->hwirq);
	pcie_write(pcie, val, AMD_MDB_TLP_IR_DISABLE_MISC);
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static void amd_mdb_intx_irq_unmask(struct irq_data *data)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(data);
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *port = &pci->pp;
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&port->lock, flags);
	val = AMD_MDB_PCIE_INTX_BIT(data->hwirq);
	pcie_write(pcie, val, AMD_MDB_TLP_IR_ENABLE_MISC);
	raw_spin_unlock_irqrestore(&port->lock, flags);
}

static struct irq_chip amd_mdb_intx_irq_chip = {
	.name		= "AMD MDB INTx",
	.irq_mask	= amd_mdb_intx_irq_mask,
	.irq_unmask	= amd_mdb_intx_irq_unmask,
};

/**
 * amd_mdb_pcie_intx_map - Set the handler for the INTx and mark IRQ
 * as valid
 * @domain: IRQ domain
 * @irq: Virtual IRQ number
 * @hwirq: HW interrupt number
 *
 * Return: Always returns 0.
 */
static int amd_mdb_pcie_intx_map(struct irq_domain *domain,
				 unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &amd_mdb_intx_irq_chip,
				 handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

/* INTx IRQ Domain operations */
static const struct irq_domain_ops amd_intx_domain_ops = {
	.map = amd_mdb_pcie_intx_map,
};

static irqreturn_t dw_pcie_rp_intx_flow(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	unsigned long val;
	int i;

	val = FIELD_GET(AMD_MDB_TLP_PCIE_INTX_MASK,
			val = pcie_read(pcie, AMD_MDB_TLP_IR_STATUS_MISC));

	for (i = 0; i < PCI_NUM_INTX; i++) {
		if (val & AMD_MDB_PCIE_INTR_INTX_ASSERT(i))
			generic_handle_domain_irq(pcie->intx_domain, i);
	}

	return IRQ_HANDLED;
}

#define _IC(x, s)[AMD_MDB_PCIE_INTR_ ## x] = { __stringify(x), s }

static const struct {
	const char	*sym;
	const char	*str;
} intr_cause[32] = {
	_IC(CMPL_TIMEOUT,	"completion timeout"),
	_IC(PM_PME_RCVD,	"PM_PME message received"),
	_IC(PME_TO_ACK_RCVD,	"PME_TO_ACK message received"),
	_IC(MISC_CORRECTABLE,	"Correctable error message"),
	_IC(NONFATAL,		"Non fatal error message"),
	_IC(FATAL,		"Fatal error message"),
};

static void amd_mdb_event_irq_mask(struct irq_data *d)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *port = &pci->pp;
	u32 val;

	raw_spin_lock(&port->lock);
	val = pcie_read(pcie, AMD_MDB_TLP_IR_STATUS_MISC);
	val &= ~BIT(d->hwirq);
	pcie_write(pcie, val, AMD_MDB_TLP_IR_STATUS_MISC);
	raw_spin_unlock(&port->lock);
}

static void amd_mdb_event_irq_unmask(struct irq_data *d)
{
	struct amd_mdb_pcie *pcie = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *port = &pci->pp;
	u32 val;

	raw_spin_lock(&port->lock);
	val = pcie_read(pcie, AMD_MDB_TLP_IR_STATUS_MISC);
	val |= BIT(d->hwirq);
	pcie_write(pcie, val, AMD_MDB_TLP_IR_STATUS_MISC);
	raw_spin_unlock(&port->lock);
}

static struct irq_chip amd_mdb_event_irq_chip = {
	.name		= "AMD MDB RC-Event",
	.irq_mask	= amd_mdb_event_irq_mask,
	.irq_unmask	= amd_mdb_event_irq_unmask,
};

static int amd_mdb_pcie_event_map(struct irq_domain *domain,
				  unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &amd_mdb_event_irq_chip,
				 handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

static const struct irq_domain_ops event_domain_ops = {
	.map = amd_mdb_pcie_event_map,
};

static irqreturn_t amd_mdb_pcie_event_flow(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	unsigned long val;
	int i;

	val = pcie_read(pcie, AMD_MDB_TLP_IR_STATUS_MISC);
	val &= ~pcie_read(pcie, AMD_MDB_TLP_IR_MASK_MISC);
	for_each_set_bit(i, &val, 32)
		generic_handle_domain_irq(pcie->mdb_domain, i);
	pcie_write(pcie, val, AMD_MDB_TLP_IR_STATUS_MISC);

	return IRQ_HANDLED;
}

static void amd_mdb_pcie_free_irq_domains(struct amd_mdb_pcie *pcie)
{
	if (pcie->intx_domain) {
		irq_domain_remove(pcie->intx_domain);
		pcie->intx_domain = NULL;
	}

	if (pcie->mdb_domain) {
		irq_domain_remove(pcie->mdb_domain);
		pcie->mdb_domain = NULL;
	}
}

static int amd_mdb_pcie_init_port(struct amd_mdb_pcie *pcie)
{
	/* Disable all TLP Interrupts */
	pcie_write(pcie, AMD_MDB_PCIE_IMR_ALL_MASK,
		   AMD_MDB_TLP_IR_DISABLE_MISC);

	/* Clear pending TLP interrupts */
	pcie_write(pcie, pcie_read(pcie, AMD_MDB_TLP_IR_STATUS_MISC) &
		   AMD_MDB_PCIE_IMR_ALL_MASK,
		   AMD_MDB_TLP_IR_STATUS_MISC);

	/* Enable all TLP Interrupts */
	pcie_write(pcie,  AMD_MDB_PCIE_IMR_ALL_MASK,
		   AMD_MDB_TLP_IR_ENABLE_MISC);

	return 0;
}

/**
 * amd_mdb_pcie_init_irq_domains - Initialize IRQ domain
 * @pcie: PCIe port information
 * @pdev: platform device
 * Return: '0' on success and error value on failure
 */
static int amd_mdb_pcie_init_irq_domains(struct amd_mdb_pcie *pcie,
					 struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node;

	/* Setup INTx */
	pcie_intc_node = of_get_next_child(node, NULL);
	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODATA;
	}

	pcie->mdb_domain = irq_domain_add_linear(pcie_intc_node, 32,
						 &event_domain_ops, pcie);
	if (!pcie->mdb_domain) {
		dev_err(dev, "Failed to add mdb_domain\n");
		goto out;
	}

	irq_domain_update_bus_token(pcie->mdb_domain, DOMAIN_BUS_NEXUS);

	pcie->intx_domain = irq_domain_add_linear(pcie_intc_node, PCI_NUM_INTX,
						  &amd_intx_domain_ops, pcie);
	if (!pcie->intx_domain) {
		dev_err(dev, "Failed to add intx_domain\n");
		goto mdb_out;
	}

	of_node_put(pcie_intc_node);
	irq_domain_update_bus_token(pcie->intx_domain, DOMAIN_BUS_WIRED);

	raw_spin_lock_init(&pp->lock);

	return 0;
mdb_out:
	amd_mdb_pcie_free_irq_domains(pcie);
out:
	of_node_put(pcie_intc_node);

	return -ENOMEM;
}

static irqreturn_t amd_mdb_pcie_intr_handler(int irq, void *args)
{
	struct amd_mdb_pcie *pcie = args;
	struct device *dev;
	struct irq_data *d;

	dev = pcie->pci.dev;

	/*
	 * In future, error reporting will be hooked to the AER subsystem.
	 * Currently, the driver prints a warning message to the user.
	 */
	d = irq_domain_get_irq_data(pcie->mdb_domain, irq);
	if (intr_cause[d->hwirq].str)
		dev_warn(dev, "%s\n", intr_cause[d->hwirq].str);
	else
		dev_warn_once(dev, "Unknown IRQ %ld\n", d->hwirq);

	return IRQ_HANDLED;
}

static int amd_mdb_setup_irq(struct amd_mdb_pcie *pcie,
			     struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int i, irq, err;

	amd_mdb_pcie_init_port(pcie);

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	for (i = 0; i < ARRAY_SIZE(intr_cause); i++) {
		if (!intr_cause[i].str)
			continue;
		irq = irq_create_mapping(pcie->mdb_domain, i);
		if (!irq) {
			dev_err(dev, "Failed to map mdb domain interrupt\n");
			return -ENOMEM;
		}
		err = devm_request_irq(dev, irq, amd_mdb_pcie_intr_handler,
				       IRQF_SHARED | IRQF_NO_THREAD,
				       intr_cause[i].sym, pcie);
		if (err) {
			dev_err(dev, "Failed to request IRQ %d\n", irq);
			return err;
		}
	}

	pcie->intx_irq = irq_create_mapping(pcie->mdb_domain,
					    AMD_MDB_PCIE_INTR_INTX);
	if (!pcie->intx_irq) {
		dev_err(dev, "Failed to map INTx interrupt\n");
		return -ENXIO;
	}

	err = devm_request_irq(dev, pcie->intx_irq,
			       dw_pcie_rp_intx_flow,
			       IRQF_SHARED | IRQF_NO_THREAD, NULL, pcie);
	if (err) {
		dev_err(dev, "Failed to request INTx IRQ %d\n", irq);
		return err;
	}

	/* Plug the main event handler */
	err = devm_request_irq(dev, pp->irq, amd_mdb_pcie_event_flow,
			       IRQF_SHARED | IRQF_NO_THREAD, "amd_mdb pcie_irq",
			       pcie);
	if (err) {
		dev_err(dev, "Failed to request event IRQ %d\n", pp->irq);
		return err;
	}

	return 0;
}

static int amd_mdb_add_pcie_port(struct amd_mdb_pcie *pcie,
				 struct platform_device *pdev)
{
	struct dw_pcie *pci = &pcie->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pcie->slcr = devm_platform_ioremap_resource_byname(pdev, "slcr");
	if (IS_ERR(pcie->slcr))
		return PTR_ERR(pcie->slcr);

	ret = amd_mdb_pcie_init_irq_domains(pcie, pdev);
	if (ret)
		return ret;

	ret = amd_mdb_setup_irq(pcie, pdev);
	if (ret) {
		dev_err(dev, "Failed to set up interrupts\n");
		goto out;
	}

	pp->ops = &amd_mdb_pcie_host_ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "Failed to initialize host\n");
		goto out;
	}

	return 0;

out:
	amd_mdb_pcie_free_irq_domains(pcie);
	return ret;
}

static int amd_mdb_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amd_mdb_pcie *pcie;
	struct dw_pcie *pci;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pci = &pcie->pci;
	pci->dev = dev;

	platform_set_drvdata(pdev, pcie);

	return amd_mdb_add_pcie_port(pcie, pdev);
}

static const struct of_device_id amd_mdb_pcie_of_match[] = {
	{
		.compatible = "amd,versal2-mdb-host",
	},
	{},
};

static struct platform_driver amd_mdb_pcie_driver = {
	.driver = {
		.name	= "amd-mdb-pcie",
		.of_match_table = amd_mdb_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = amd_mdb_pcie_probe,
};
builtin_platform_driver(amd_mdb_pcie_driver);
