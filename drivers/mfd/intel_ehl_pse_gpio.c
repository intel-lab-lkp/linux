// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel MFD driver for Elkhart Lake - Programmable Service Engine
 * (PSE) GPIO & TIO
 *
 * Copyright (c) 2025 Intel Corporation
 *
 * Intel Elkhart Lake PSE includes two PCI devices that expose two
 * different capabilities of GPIO and Timed I/O as a single PCI
 * function through shared MMIO.
 */

#include <linux/array_size.h>
#include <linux/ioport.h>
#include <linux/mfd/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/stddef.h>

#define PSE_GPIO_OFFSET		0x0000
#define PSE_GPIO_SIZE		0x0134

#define PSE_TIO_OFFSET		0x1000
#define PSE_TIO_SIZE		0x06B0

static struct resource ehl_pse_gpio_resources[] = {
	DEFINE_RES_MEM(PSE_GPIO_OFFSET, PSE_GPIO_SIZE),
	DEFINE_RES_IRQ(0),
};

static struct resource ehl_pse_tio_resources[] = {
	DEFINE_RES_MEM(PSE_TIO_OFFSET, PSE_TIO_SIZE),
	DEFINE_RES_IRQ(1),
};

static struct mfd_cell ehl_pse_gpio_devs[] = {
	{
		.name = "gpio-elkhartlake",
		.num_resources = ARRAY_SIZE(ehl_pse_gpio_resources),
		.resources = ehl_pse_gpio_resources,
		.ignore_resource_conflicts = true,
	},
	{
		.name = "pps-gen-tio",
		.num_resources = ARRAY_SIZE(ehl_pse_tio_resources),
		.resources = ehl_pse_tio_resources,
		.ignore_resource_conflicts = true,
	},
};

static int ehl_pse_gpio_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	int ret;

	ret = pcim_enable_device(pci);
	if (ret)
		return ret;

	pci_set_master(pci);

	ret = pci_alloc_irq_vectors(pci, 2, 2, PCI_IRQ_ALL_TYPES);
	if (ret < 0)
		return ret;

	ret = mfd_add_devices(&pci->dev, PLATFORM_DEVID_AUTO, ehl_pse_gpio_devs,
			      ARRAY_SIZE(ehl_pse_gpio_devs), pci_resource_n(pci, 0),
			      pci_irq_vector(pci, 0), NULL);
	if (ret)
		pci_free_irq_vectors(pci);

	return ret;
}

static void ehl_pse_gpio_remove(struct pci_dev *pdev)
{
	mfd_remove_devices(&pdev->dev);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id ehl_pse_gpio_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x4b88) },
	{ PCI_VDEVICE(INTEL, 0x4b89) },
	{}
};
MODULE_DEVICE_TABLE(pci, ehl_pse_gpio_ids);

static struct pci_driver ehl_pse_gpio_driver = {
	.probe		= ehl_pse_gpio_probe,
	.remove		= ehl_pse_gpio_remove,
	.id_table	= ehl_pse_gpio_ids,
	.name		= "ehl_pse_gpio",
};
module_pci_driver(ehl_pse_gpio_driver);

MODULE_AUTHOR("Raymond Tan <raymond.tan@intel.com>");
MODULE_AUTHOR("Raag Jadav <raag.jadav@intel.com>");
MODULE_DESCRIPTION("Intel MFD for Elkhart Lake PSE GPIO & TIO");
MODULE_LICENSE("GPL");
