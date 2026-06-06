// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI PCI device code
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "sdxi.h"

enum sdxi_mmio_bars {
	SDXI_PCI_BAR_CTL_REGS = 0,
	SDXI_PCI_BAR_DOORBELL = 2,
};

static struct pci_dev *sdxi_to_pci_dev(const struct sdxi_dev *sdxi)
{
	return to_pci_dev(sdxi->dev);
}

static int sdxi_pci_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi_to_pci_dev(sdxi);
	struct device *dev = &pdev->dev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable device\n");

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	sdxi->ctrl_regs = pcim_iomap_region(pdev, SDXI_PCI_BAR_CTL_REGS,
					    KBUILD_MODNAME);
	if (IS_ERR(sdxi->ctrl_regs))
		return dev_err_probe(dev, PTR_ERR(sdxi->ctrl_regs),
				     "failed to map control registers\n");

	sdxi->dbs = pcim_iomap_region(pdev, SDXI_PCI_BAR_DOORBELL,
				      KBUILD_MODNAME);
	if (IS_ERR(sdxi->dbs))
		return dev_err_probe(dev, PTR_ERR(sdxi->dbs),
				     "failed to map doorbell region\n");

	pci_set_master(pdev);
	return 0;
}

static const struct sdxi_bus_ops sdxi_pci_ops = {
	.init = sdxi_pci_init,
};

static int sdxi_pci_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	return sdxi_register(&pdev->dev, &sdxi_pci_ops);
}

static void sdxi_pci_remove(struct pci_dev *pdev)
{
	pci_disable_sriov(pdev);
	sdxi_unregister(&pdev->dev);
}

static const struct pci_device_id sdxi_id_table[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_ACCELERATOR_SDXI, 0xffffff) },
	{ }
};
MODULE_DEVICE_TABLE(pci, sdxi_id_table);

static struct pci_driver sdxi_driver = {
	.name = "sdxi",
	.id_table = sdxi_id_table,
	.probe = sdxi_pci_probe,
	.remove = sdxi_pci_remove,
	.sriov_configure = pci_sriov_configure_simple,
};

MODULE_IMPORT_NS("SDXI");
MODULE_AUTHOR("Wei Huang");
MODULE_AUTHOR("Nathan Lynch");
MODULE_DESCRIPTION("SDXI PCIe interface driver");
MODULE_LICENSE("GPL");
module_pci_driver(sdxi_driver);
