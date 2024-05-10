// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AE4DMA driver
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Basavaraj Natikar <Basavaraj.Natikar@amd.com>
 */

#include "ae4dma.h"

static int ae4_get_msi_irq(struct ae4_device *ae4)
{
	struct pt_device *pt = &ae4->pt;
	struct device *dev = pt->dev;
	struct pci_dev *pdev;
	int ret, i;

	pdev = to_pci_dev(dev);
	ret = pci_enable_msi(pdev);
	if (ret)
		return ret;

	for (i = 0; i < MAX_AE4_HW_QUEUES; i++)
		ae4->ae4_irq[i] = pdev->irq;

	return 0;
}

static int ae4_get_msix_irqs(struct ae4_device *ae4)
{
	struct ae4_msix *ae4_msix = ae4->ae4_msix;
	struct pt_device *pt = &ae4->pt;
	struct device *dev = pt->dev;
	struct pci_dev *pdev;
	int v, i, ret;

	pdev = to_pci_dev(dev);

	for (v = 0; v < ARRAY_SIZE(ae4_msix->msix_entry); v++)
		ae4_msix->msix_entry[v].entry = v;

	ret = pci_enable_msix_range(pdev, ae4_msix->msix_entry, 1, v);
	if (ret < 0)
		return ret;

	ae4_msix->msix_count = ret;

	for (i = 0; i < MAX_AE4_HW_QUEUES; i++)
		ae4->ae4_irq[i] = ae4_msix->msix_entry[i].vector;

	return 0;
}

static int ae4_get_irqs(struct ae4_device *ae4)
{
	struct pt_device *pt = &ae4->pt;
	struct device *dev = pt->dev;
	int ret;

	ret = ae4_get_msix_irqs(ae4);
	if (!ret)
		return 0;

	/* Couldn't get MSI-X vectors, try MSI */
	dev_err(dev, "could not enable MSI-X (%d), trying MSI\n", ret);
	ret = ae4_get_msi_irq(ae4);
	if (!ret)
		return 0;

	/* Couldn't get MSI interrupt */
	dev_err(dev, "could not enable MSI (%d)\n", ret);

	return ret;
}

static void ae4_free_irqs(struct ae4_device *ae4)
{
	struct ae4_msix *ae4_msix;
	struct pci_dev *pdev;
	struct pt_device *pt;
	struct device *dev;
	int i;

	if (ae4) {
		pt = &ae4->pt;
		dev = pt->dev;
		pdev = to_pci_dev(dev);

		ae4_msix = ae4->ae4_msix;
		if (ae4_msix && ae4_msix->msix_count)
			pci_disable_msix(pdev);
		else if (pdev->irq)
			pci_disable_msi(pdev);

		for (i = 0; i < MAX_AE4_HW_QUEUES; i++)
			ae4->ae4_irq[i] = 0;
	}
}

static void ae4_deinit(struct ae4_device *ae4)
{
	ae4_free_irqs(ae4);
}

static int ae4_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct ae4_device *ae4;
	struct pt_device *pt;
	int bar_mask;
	int ret = 0;

	ae4 = devm_kzalloc(dev, sizeof(*ae4), GFP_KERNEL);
	if (!ae4)
		return -ENOMEM;

	ae4->ae4_msix = devm_kzalloc(dev, sizeof(struct ae4_msix), GFP_KERNEL);
	if (!ae4->ae4_msix)
		return -ENOMEM;

	ret = pcim_enable_device(pdev);
	if (ret)
		goto ae4_error;

	bar_mask = pci_select_bars(pdev, IORESOURCE_MEM);
	ret = pcim_iomap_regions(pdev, bar_mask, "ae4dma");
	if (ret)
		goto ae4_error;

	pt = &ae4->pt;
	pt->dev = dev;
	pt->ver = AE4_DMA_VERSION;

	pt->io_regs = pcim_iomap_table(pdev)[0];
	if (!pt->io_regs) {
		ret = -ENOMEM;
		goto ae4_error;
	}

	ret = ae4_get_irqs(ae4);
	if (ret)
		goto ae4_error;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));
	if (ret) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret)
			goto ae4_error;
	}

	dev_set_drvdata(dev, ae4);

	ret = ae4_core_init(ae4);
	if (ret)
		goto ae4_error;

	return 0;

ae4_error:
	ae4_deinit(ae4);

	return ret;
}

static void ae4_pci_remove(struct pci_dev *pdev)
{
	struct ae4_device *ae4 = dev_get_drvdata(&pdev->dev);

	ae4_destroy_work(ae4);
	ae4_deinit(ae4);
}

static const struct pci_device_id ae4_pci_table[] = {
	{ PCI_VDEVICE(AMD, 0x14C8), },
	{ PCI_VDEVICE(AMD, 0x14DC), },
	{ PCI_VDEVICE(AMD, 0x149B), },
	/* Last entry must be zero */
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, ae4_pci_table);

static struct pci_driver ae4_pci_driver = {
	.name = "ae4dma",
	.id_table = ae4_pci_table,
	.probe = ae4_pci_probe,
	.remove = ae4_pci_remove,
};

module_pci_driver(ae4_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD AE4DMA driver");
