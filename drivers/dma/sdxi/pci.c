// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI PCI device code
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iomap.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci-ats.h>
#include <linux/pci.h>

#include "mmio.h"
#include "sdxi.h"

/*
 * SDXI devices signal message 0 on error conditions, see "Error
 * Logging Control and Status Registers".
 */
#define ERROR_IRQ_MSG 0

/* MMIO BARs */
#define MMIO_CTL_REGS_BAR		0x0
#define MMIO_DOORBELL_BAR		0x2

static struct pci_dev *sdxi_to_pci_dev(const struct sdxi_dev *sdxi)
{
	return to_pci_dev(sdxi_to_dev(sdxi));
}

static int sdxi_pci_irq_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi_to_pci_dev(sdxi);
	int msi_count;
	int ret;

	/* 1st irq for error + 1 for each context */
	msi_count = sdxi->max_cxts + 1;

	ret = pci_alloc_irq_vectors(pdev, 1, msi_count,
				    PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		sdxi_err(sdxi, "alloc MSI/MSI-X vectors failed\n");
		return ret;
	}

	sdxi->error_irq = pci_irq_vector(pdev, ERROR_IRQ_MSG);

	sdxi_dbg(sdxi, "allocated %d irq vectors", ret);

	return 0;
}

static void sdxi_pci_irq_exit(struct sdxi_dev *sdxi)
{
	pci_free_irq_vectors(sdxi_to_pci_dev(sdxi));
}

static int sdxi_pci_map(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi_to_pci_dev(sdxi);
	int bars, ret;

	bars = 1 << MMIO_CTL_REGS_BAR | 1 << MMIO_DOORBELL_BAR;
	ret = pcim_iomap_regions(pdev, bars, SDXI_DRV_NAME);
	if (ret) {
		sdxi_err(sdxi, "pcim_iomap_regions failed (%d)\n", ret);
		return ret;
	}

	sdxi->dbs_bar = pci_resource_start(pdev, MMIO_DOORBELL_BAR);

	/* FIXME: pcim_iomap_table may return NULL, and it's deprecated. */
	sdxi->ctrl_regs = pcim_iomap_table(pdev)[MMIO_CTL_REGS_BAR];
	sdxi->dbs = pcim_iomap_table(pdev)[MMIO_DOORBELL_BAR];
	if (!sdxi->ctrl_regs || !sdxi->dbs) {
		sdxi_err(sdxi, "pcim_iomap_table failed\n");
		return -EINVAL;
	}

	return 0;
}

static void sdxi_pci_unmap(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi_to_pci_dev(sdxi);

	pcim_iounmap(pdev, sdxi->ctrl_regs);
	pcim_iounmap(pdev, sdxi->dbs);
}

static int sdxi_pci_init(struct sdxi_dev *sdxi)
{
	struct pci_dev *pdev = sdxi_to_pci_dev(sdxi);
	struct device *dev = &pdev->dev;
	int dma_bits = 64;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret) {
		sdxi_err(sdxi, "pcim_enbale_device failed\n");
		return ret;
	}

	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(dma_bits));
	if (ret) {
		sdxi_err(sdxi, "failed to set DMA mask & coherent bits\n");
		return ret;
	}

	ret = sdxi_pci_map(sdxi);
	if (ret) {
		sdxi_err(sdxi, "failed to map device IO resources\n");
		return ret;
	}

	return 0;
}

static void sdxi_pci_exit(struct sdxi_dev *sdxi)
{
	sdxi_pci_unmap(sdxi);
}

static struct sdxi_dev *sdxi_device_alloc(struct device *dev)
{
	struct sdxi_dev *sdxi;

	sdxi = kzalloc(sizeof(*sdxi), GFP_KERNEL);
	if (!sdxi)
		return NULL;

	sdxi->dev = dev;

	mutex_init(&sdxi->cxt_lock);

	return sdxi;
}

static void sdxi_device_free(struct sdxi_dev *sdxi)
{
	kfree(sdxi);
}

static const struct sdxi_dev_ops sdxi_pci_dev_ops = {
	.irq_init = sdxi_pci_irq_init,
	.irq_exit = sdxi_pci_irq_exit,
};

static int sdxi_pci_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct sdxi_dev *sdxi;
	int err;

	sdxi = sdxi_device_alloc(dev);
	if (!sdxi)
		return -ENOMEM;

	pci_set_drvdata(pdev, sdxi);

	err = sdxi_pci_init(sdxi);
	if (err)
		goto free_sdxi;

	err = sdxi_device_init(sdxi, &sdxi_pci_dev_ops);
	if (err)
		goto pci_exit;

	return 0;

pci_exit:
	sdxi_pci_exit(sdxi);
free_sdxi:
	sdxi_device_free(sdxi);

	return err;
}

static void sdxi_pci_remove(struct pci_dev *pdev)
{
	struct sdxi_dev *sdxi = pci_get_drvdata(pdev);

	sdxi_device_exit(sdxi);
	sdxi_pci_exit(sdxi);
	sdxi_device_free(sdxi);
}

static const struct pci_device_id sdxi_id_table[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_ACCELERATOR_SDXI, 0xffffff) },
	{0, }
};
MODULE_DEVICE_TABLE(pci, sdxi_id_table);

static struct pci_driver sdxi_driver = {
	.name = "sdxi",
	.id_table = sdxi_id_table,
	.probe = sdxi_pci_probe,
	.remove = sdxi_pci_remove,
	.sriov_configure = pci_sriov_configure_simple,
};

module_pci_driver(sdxi_driver);
