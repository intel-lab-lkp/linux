// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) Tehuti Networks Ltd. */

#include "tn40.h"

static int bdx_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int ret;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "failed to set DMA mask.\n");
			goto err_disable_device;
		}
	}
	return 0;
err_disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void bdx_remove(struct pci_dev *pdev)
{
	pci_disable_device(pdev);
}

static const struct pci_device_id bdx_id_table[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_TEHUTI, 0x3015) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_DLINK, 0x4d00) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_ASUSTEK, 0x8709) },
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_TEHUTI, 0x4022,
			 PCI_VENDOR_ID_EDIMAX, 0x8103) },
	{ }
};

static struct pci_driver bdx_driver = {
	.name = BDX_DRV_NAME,
	.id_table = bdx_id_table,
	.probe = bdx_probe,
	.remove = bdx_remove,
};

module_pci_driver(bdx_driver);

MODULE_DEVICE_TABLE(pci, bdx_id_table);
MODULE_AUTHOR("Tehuti networks");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tehuti Network TN30xx Driver");
MODULE_VERSION(BDX_DRV_VERSION);
