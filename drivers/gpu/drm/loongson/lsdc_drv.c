// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <linux/aperture.h>
#include <linux/component.h>
#include <linux/pci.h>
#include <linux/vgaarb.h>

#include "loongson_module.h"
#include "loongson_drv.h"
#include "lsdc_drv.h"

extern const struct component_ops lsdc_pci_component_ops;

static int lsdc_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	const struct lsdc_desc *descp;
	struct lsdc_device *lsdc;
	int ret;

	descp = lsdc_device_probe(pdev, ent->driver_data);
	if (IS_ERR_OR_NULL(descp))
		return -ENODEV;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Found %s, revision: %u\n",
		 to_loongson_gfx(descp)->model, pdev->revision);

	lsdc = devm_kzalloc(&pdev->dev, sizeof(*lsdc), GFP_KERNEL);
	if (!lsdc)
		return -ENOMEM;

	/* Bar 0 of the DC device contains the MMIO register's base address */
	lsdc->reg_base = pcim_iomap(pdev, 0, 0);
	if (!lsdc->reg_base)
		return -ENODEV;

	lsdc->descp = descp;
	spin_lock_init(&lsdc->reglock);

	pci_set_drvdata(pdev, lsdc);

	ret = lsdc_i2c_preinit(&pdev->dev, descp);
	if (ret)
		return ret;

	ret = component_add(&pdev->dev, &lsdc_pci_component_ops);
	if (ret)
		return ret;

	ret = lsdc_output_preinit(&pdev->dev, descp);
	if (ret)
		return ret;

	ret = loongson_device_preinit(&pdev->dev);
	if (ret)
		return ret;

	return 0;
}

static void lsdc_pci_remove(struct pci_dev *pdev)
{
	component_del(&pdev->dev, &lsdc_pci_component_ops);
}

static int lsdc_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_save_state(pdev);
	/* Shut down the device */
	pci_disable_device(pdev);
	pci_set_power_state(pdev, PCI_D3hot);

	return 0;
}

static int lsdc_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	pci_set_power_state(pdev, PCI_D0);

	pci_restore_state(pdev);

	if (pcim_enable_device(pdev))
		return -EIO;

	return 0;
}

static const struct dev_pm_ops lsdc_pm_ops = {
	.suspend = lsdc_pm_suspend,
	.resume = lsdc_pm_resume,
};

static const struct pci_device_id lsdc_pciid_list[] = {
	{PCI_VDEVICE(LOONGSON, 0x7a06), CHIP_LS7A1000},
	{PCI_VDEVICE(LOONGSON, 0x7a36), CHIP_LS7A2000},
	{ }
};

struct pci_driver lsdc_pci_driver = {
	.name = "loongson.lsdc",
	.id_table = lsdc_pciid_list,
	.probe = lsdc_pci_probe,
	.remove = lsdc_pci_remove,
	.driver.pm = &lsdc_pm_ops,
};

MODULE_DEVICE_TABLE(pci, lsdc_pciid_list);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
