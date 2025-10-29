// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel Elkhart Lake Programmable Service Engine (PSE) I/O
 *
 * Copyright (c) 2025 Intel Corporation.
 *
 * Author: Raag Jadav <raag.jadav@intel.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/dev_printk.h>
#include <linux/device/devres.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gfp_types.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <linux/ehl_pse_io_aux.h>

#define EHL_PSE_IO_DEV_OFFSET	SZ_4K
#define EHL_PSE_IO_DEV_SIZE	SZ_4K

static void ehl_pse_io_dev_release(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct ehl_pse_io_dev *io_dev = auxiliary_dev_to_ehl_pse_io_dev(aux_dev);

	kfree(io_dev);
}

static int ehl_pse_io_dev_add(struct pci_dev *pci, const char *name, int idx)
{
	struct auxiliary_device *aux_dev;
	struct device *dev = &pci->dev;
	struct ehl_pse_io_dev *io_dev;
	resource_size_t start;
	int ret;

	io_dev = kzalloc(sizeof(*io_dev), GFP_KERNEL);
	if (!io_dev)
		return -ENOMEM;

	start = pci_resource_start(pci, 0);
	io_dev->irq = pci_irq_vector(pci, idx);
	io_dev->mem = DEFINE_RES_MEM(start + (EHL_PSE_IO_DEV_OFFSET * idx), EHL_PSE_IO_DEV_SIZE);

	aux_dev = &io_dev->aux_dev;
	aux_dev->name = name;
	aux_dev->id = (pci_domain_nr(pci->bus) << 16) | pci_dev_id(pci);
	aux_dev->dev.parent = dev;
	aux_dev->dev.release = ehl_pse_io_dev_release;

	ret = auxiliary_device_init(aux_dev);
	if (ret)
		goto free_io_dev;

	ret = __auxiliary_device_add(aux_dev, dev->driver->name);
	if (ret)
		goto uninit_aux_dev;

	return 0;

uninit_aux_dev:
	/* io_dev will be freed with the put_device() and .release sequence */
	auxiliary_device_uninit(aux_dev);
free_io_dev:
	kfree(io_dev);
	return ret;
}

static int ehl_pse_io_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	int ret;

	ret = pcim_enable_device(pci);
	if (ret)
		return ret;

	pci_set_master(pci);

	ret = pci_alloc_irq_vectors(pci, 2, 2, PCI_IRQ_MSI);
	if (ret < 0)
		return ret;

	ret = ehl_pse_io_dev_add(pci, EHL_PSE_GPIO_NAME, 0);
	if (ret)
		return ret;

	return ehl_pse_io_dev_add(pci, EHL_PSE_TIO_NAME, 1);
}

static int ehl_pse_io_dev_destroy(struct device *dev, void *data)
{
	auxiliary_device_destroy(to_auxiliary_dev(dev));

	return 0;
}

static void ehl_pse_io_remove(struct pci_dev *pci)
{
	struct device *dev = &pci->dev;

	device_for_each_child_reverse(dev, NULL, ehl_pse_io_dev_destroy);
}

static const struct pci_device_id ehl_pse_io_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x4b88) },
	{ PCI_VDEVICE(INTEL, 0x4b89) },
	{ }
};
MODULE_DEVICE_TABLE(pci, ehl_pse_io_ids);

static struct pci_driver ehl_pse_io_driver = {
	.name		= EHL_PSE_IO_NAME,
	.id_table	= ehl_pse_io_ids,
	.probe		= ehl_pse_io_probe,
	.remove		= ehl_pse_io_remove,
};
module_pci_driver(ehl_pse_io_driver);

MODULE_AUTHOR("Raag Jadav <raag.jadav@intel.com>");
MODULE_DESCRIPTION("Intel Elkhart Lake PSE I/O driver");
MODULE_LICENSE("GPL");
