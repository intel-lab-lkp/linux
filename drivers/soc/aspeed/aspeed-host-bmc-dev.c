// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) ASPEED Technology Inc.

#include <linux/init.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>

#define BMC_MULTI_MSI	32
#define BMC_MSI_IDX_BASE	4

#define DRIVER_NAME "ASPEED BMC DEVICE"

#define VUART_MAX_PARMS		2

#define BAR_MEM 0
#define BAR_MSG 1
#define BAR_MAX 2

struct bar {
	unsigned long bar_base;
	unsigned long bar_size;
	void __iomem *bar_ioremap;
};

struct aspeed_pci_bmc_dev {
	struct device *dev;

	struct bar bars[BAR_MAX];
	int lines[VUART_MAX_PARMS];

	int legacy_irq;
};

static uint16_t vuart_ioport[VUART_MAX_PARMS];
static uint16_t vuart_sirq[VUART_MAX_PARMS];

static int aspeed_pci_host_bmc_device_probe(struct pci_dev *pdev,
		const struct pci_device_id *ent)
{
	struct uart_8250_port uart[VUART_MAX_PARMS];
	struct device *dev = &pdev->dev;
	struct aspeed_pci_bmc_dev *pci_bmc_dev;
	int rc = 0;
	int i = 0;
	int nr_entries;
	u16 config_cmd_val;

	pci_bmc_dev = kzalloc(sizeof(*pci_bmc_dev), GFP_KERNEL);
	if (!pci_bmc_dev) {
		rc = -ENOMEM;
		dev_err(dev, "kmalloc() returned NULL memory.\n");
		goto out_err;
	}

	rc = pcim_enable_device(pdev);
	if (rc != 0) {
		dev_err(dev, "pcim_enable_device() returned error %d\n", rc);
		goto out_free0;
	}

	/* set PCI host mastering  */
	pci_set_master(pdev);

	/*
	 * Try to allocate max MSI. If multiple MSI is not possible then use
	 * the legacy interrupt. Note: PowerPC doesn't support multiple MSI.
	 */
	nr_entries = pci_alloc_irq_vectors(pdev, BMC_MULTI_MSI, BMC_MULTI_MSI,
				PCI_IRQ_MSIX | PCI_IRQ_MSI);

	if (nr_entries < 0) {
		pci_bmc_dev->legacy_irq = 1;
		pci_read_config_word(pdev, PCI_COMMAND, &config_cmd_val);
		config_cmd_val &= ~PCI_COMMAND_INTX_DISABLE;
		pci_write_config_word((struct pci_dev *)pdev, PCI_COMMAND, config_cmd_val);

	} else {
		pci_bmc_dev->legacy_irq = 0;
		pci_read_config_word(pdev, PCI_COMMAND, &config_cmd_val);
		config_cmd_val |= PCI_COMMAND_INTX_DISABLE;
		pci_write_config_word((struct pci_dev *)pdev, PCI_COMMAND, config_cmd_val);
		rc = pci_irq_vector(pdev, BMC_MSI_IDX_BASE);
		if (rc < 0) {
			dev_err(dev, "pci_irq_vector() returned error %d msi=%u msix=%u\n",
				-rc, pdev->msi_enabled, pdev->msix_enabled);
			goto out_free1;
		}
		pdev->irq = rc;
	}

	/* Get access to the BARs */
	for (i = 0; i < BAR_MAX; i++) {
		rc = pci_request_region(pdev, i, DRIVER_NAME);
		if (rc < 0) {
			dev_err(dev, "pci_request_region(%d) returned error %d\n", i, rc);
			goto out_unreg;
		}

		pci_bmc_dev->bars[i].bar_base = pci_resource_start(pdev, i);
		pci_bmc_dev->bars[i].bar_size = pci_resource_len(pdev, i);
		pci_bmc_dev->bars[i].bar_ioremap = pci_ioremap_bar(pdev, i);
		if (pci_bmc_dev->bars[i].bar_ioremap == NULL) {
			dev_err(dev, "pci_ioremap_bar(%d) failed\n", i);
			rc = -ENOMEM;
			goto out_unreg;
		}
	}

	/* ERRTA40: dummy read */
	(void)__raw_readl((void __iomem *)pci_bmc_dev->bars[BAR_MSG].bar_ioremap);

	pci_set_drvdata(pdev, pci_bmc_dev);

	/* setup VUART */
	memset(uart, 0, sizeof(uart));

	for (i = 0; i < VUART_MAX_PARMS; i++) {
		vuart_ioport[i] = 0x3F8 - (i * 0x100);
		vuart_sirq[i] = 0x10 + 4 - i - BMC_MSI_IDX_BASE;
		uart[i].port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
		uart[i].port.uartclk = 115200 * 16;
		pci_bmc_dev->lines[i] = -1;

		if (pci_bmc_dev->legacy_irq) {
			uart[i].port.irq = pdev->irq;
		} else {
			rc = pci_irq_vector(pdev, vuart_sirq[i]);
			if (rc < 0) {
				dev_err(dev,
					"pci_irq_vector() returned error %d msi=%u msix=%u\n",
					-rc, pdev->msi_enabled, pdev->msix_enabled);
				goto out_unreg;
			}
			uart[i].port.irq = rc;
		}
		uart[i].port.dev = dev;
		uart[i].port.iotype = UPIO_MEM32;
		uart[i].port.iobase = 0;
		uart[i].port.mapbase =
				pci_bmc_dev->bars[BAR_MSG].bar_base + (vuart_ioport[i] << 2);
		uart[i].port.membase =
				pci_bmc_dev->bars[BAR_MSG].bar_ioremap + (vuart_ioport[i] << 2);
		uart[i].port.type = PORT_16550A;
		uart[i].port.flags |= (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE);
		uart[i].port.regshift = 2;

		rc = serial8250_register_8250_port(&uart[i]);
		if (rc < 0) {
			dev_err(dev,
				"cannot setup VUART@%xh over PCIe, rc=%d\n",
				vuart_ioport[i], -rc);
			goto out_unreg;
		}
		pci_bmc_dev->lines[i] = rc;
	}

	return 0;

out_unreg:
	for (i = 0; i < VUART_MAX_PARMS; i++) {
		if (pci_bmc_dev->lines[i] >= 0)
			serial8250_unregister_port(pci_bmc_dev->lines[i]);
	}

	pci_release_regions(pdev);
out_free1:
	if (pci_bmc_dev->legacy_irq)
		free_irq(pdev->irq, pdev);
	else
		pci_free_irq_vectors(pdev);

	pci_clear_master(pdev);
out_free0:
	kfree(pci_bmc_dev);
out_err:

	return rc;
}

static void aspeed_pci_host_bmc_device_remove(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	int i;

	/* Unregister ports */
	for (i = 0; i < VUART_MAX_PARMS; i++) {
		if (pci_bmc_dev->lines[i] >= 0)
			serial8250_unregister_port(pci_bmc_dev->lines[i]);
	}

	if (pci_bmc_dev->legacy_irq)
		free_irq(pdev->irq, pdev);
	else
		pci_free_irq_vectors(pdev);

	pci_release_regions(pdev);
	pci_clear_master(pdev);
	kfree(pci_bmc_dev);
}

/**
 * This table holds the list of (VendorID,DeviceID) supported by this driver
 *
 */
static struct pci_device_id aspeed_host_bmc_dev_pci_ids[] = {
	{ PCI_DEVICE(0x1A03, 0x2402), },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, aspeed_host_bmc_dev_pci_ids);

static struct pci_driver aspeed_host_bmc_dev_driver = {
	.name		= DRIVER_NAME,
	.id_table	= aspeed_host_bmc_dev_pci_ids,
	.probe		= aspeed_pci_host_bmc_device_probe,
	.remove		= aspeed_pci_host_bmc_device_remove,
};

static int __init aspeed_host_bmc_device_init(void)
{
	int ret;

	/* register pci driver */
	ret = pci_register_driver(&aspeed_host_bmc_dev_driver);
	if (ret < 0) {
		pr_err("pci-driver: can't register pci driver\n");
		return ret;
	}

	return 0;

}

static void aspeed_host_bmc_device_exit(void)
{
	/* unregister pci driver */
	pci_unregister_driver(&aspeed_host_bmc_dev_driver);
}

late_initcall(aspeed_host_bmc_device_init);
module_exit(aspeed_host_bmc_device_exit);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED Host BMC DEVICE Driver");
MODULE_LICENSE("GPL");
