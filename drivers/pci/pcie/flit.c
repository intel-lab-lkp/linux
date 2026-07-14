// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Service driver for PCIe Flit Logging Capability
 *
 * Copyright (c) 2026, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Authors:	Avadhut Naik <Avadhut.Naik@amd.com>
 *		Yazen Ghannam <Yazen.Ghannam@amd.com>
 */

#define pr_fmt(fmt) "Flit: " fmt
#define dev_fmt pr_fmt

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <ras/ras_event.h>
#include "portdrv.h"
#include "../pci.h"

void pci_flit_init(struct pci_dev *pdev)
{
	u16 cap;

	if (pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &cap))
		return;

	if (!(cap & PCI_EXP_FLAGS_FLIT))
		return;

	pdev->flit_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_FLIT);
	if (pdev->flit_cap)
		pci_dbg(pdev, "Flit Mode Error Logging Capability present.\n");
}

static void flit_cntr_enable(struct pci_dev *pdev)
{
	u16 flit = pdev->flit_cap;
	u16 reg;

	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, &reg);
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, reg);

	/*
	 * NOTE: The "Set Events to Count" and "Trigger Event Count" fields
	 * are left as set by the platform.
	 */
	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, &reg);
	reg |= PCI_FLIT_ERR_CNTR_EN;
	reg |= PCI_FLIT_ERR_CNTR_INTR_EN;
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, reg);
}

static void flit_cntr_disable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	u16 ctrl, flit = pdev->flit_cap;

	/* Disable both Error Counter and Interrupt generation */
	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, &ctrl);
	ctrl &= ~(PCI_FLIT_ERR_CNTR_EN | PCI_FLIT_ERR_CNTR_INTR_EN);
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, ctrl);
}

static irqreturn_t flit_isr(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	struct pci_dev *pdev = dev->port;
	u16 flit = pdev->flit_cap;
	u16 cntr_ctrl, cntr_sta;
	u32 err_log1, err_log2;

	/* Read and log Counter and Error Log Registers. */
	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, &cntr_ctrl);
	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, &cntr_sta);

	pci_info(pdev, HW_ERR "Counter Control: 0x%04x Counter Status: 0x%04x\n", cntr_ctrl, cntr_sta);

	do {
		pci_read_config_dword(pdev, flit + PCI_FLIT_ERR_LOG1, &err_log1);

		if (!(err_log1 & PCI_FLIT_ERR_LOG_VALID))
			break;

		pci_read_config_dword(pdev, flit + PCI_FLIT_ERR_LOG2, &err_log2);
		pci_info(pdev, HW_ERR "  Error Log1: 0x%08x Error Log2: 0x%08x\n", err_log1, err_log2);
		trace_flit_event(pci_name(pdev), cntr_ctrl, cntr_sta, err_log1, err_log2);

		pci_write_config_dword(pdev, flit + PCI_FLIT_ERR_LOG1, err_log1);
	} while (err_log1 & PCI_FLIT_ERR_LOG_MORE);

	/*
	 * Re-enabling the counter is the interrupt acknowledgment.
	 *
	 * The status bit checked in flit_irq() (Interrupt Generated based on
	 * Trigger Event Count) is cleared by a 0->1 transition of Flit Error
	 * Counter Enable, per PCIe r6.4 Table 7-96.
	 */
	cntr_ctrl |= PCI_FLIT_ERR_CNTR_EN;
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, cntr_ctrl);
	return IRQ_HANDLED;
}

static irqreturn_t flit_irq(int irq, void *context)
{
	struct pcie_device *dev = (struct pcie_device *)context;
	struct pci_dev *pdev = dev->port;
	u16 flit = pdev->flit_cap;
	u16 cntr_sta, cntr_ctrl;

	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_STA, &cntr_sta);
	if (!(cntr_sta & PCI_FLIT_INTR_GEN_CNTR))
		return IRQ_NONE;

	pci_read_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, &cntr_ctrl);
	cntr_ctrl &= ~PCI_FLIT_ERR_CNTR_EN;
	pci_write_config_word(pdev, flit + PCI_FLIT_ERR_CNTR_CTRL, cntr_ctrl);
	return IRQ_WAKE_THREAD;
}

static int flit_probe(struct pcie_device *dev)
{
	struct device *device = &dev->device;
	struct pci_dev *pdev = dev->port;
	int status;

	status = devm_request_threaded_irq(device, dev->irq, flit_irq,
					   flit_isr, IRQF_SHARED,
					   "pcie-flit", dev);
	if (status) {
		pci_warn(pdev, "request Flit IRQ%d failed: %d\n", dev->irq, status);
		return status;
	}

	flit_cntr_enable(pdev);
	pci_info(pdev, "enabled with IRQ %d\n", dev->irq);

	return 0;
}

static int flit_cntr_suspend(struct pcie_device *dev)
{
	flit_cntr_disable(dev);
	return 0;
}

static int flit_cntr_resume(struct pcie_device *dev)
{
	flit_cntr_enable(dev->port);
	return 0;
}

static void flit_cntr_remove(struct pcie_device *dev)
{
	flit_cntr_disable(dev);
}

static struct pcie_port_service_driver flitdriver = {
	.name		= "flit",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_FLIT,
	.probe		= flit_probe,
	.suspend	= flit_cntr_suspend,
	.resume		= flit_cntr_resume,
	.remove		= flit_cntr_remove,
};

int __init pcie_flit_init(void)
{
	return pcie_port_service_register(&flitdriver);
}
