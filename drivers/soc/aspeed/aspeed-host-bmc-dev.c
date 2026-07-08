// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) ASPEED Technology Inc.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>

#define BMC_MULTI_MSI	32
#define PCI_BMC_DEVICE_ID 0x2402

#define DRIVER_NAME "aspeed-host-bmc-dev"

enum aspeed_platform_id {
	ASPEED,
};

static const int vuart_msi_index[2] = { 16, 17 };
static const int vuart_port_addr[2] = {0x3f8, 0x2f8};

struct aspeed_pci_bmc_dev {
	unsigned long message_bar_base;

	struct uart_8250_port uart[2];
	int uart_line[2];
};

static int aspeed_pci_bmc_device_setup_vuart(struct pci_dev *pdev, int idx)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct uart_8250_port *uart = &pci_bmc_dev->uart[idx];
	u16 vuart_ioport;
	int ret;

	/* Assign the line to non-exist device before everything is setup */
	pci_bmc_dev->uart_line[idx] = -ENOENT;

	vuart_ioport = vuart_port_addr[idx];
	/* ASPEED BMC device shift addresses by 2 to the left */
	vuart_ioport = vuart_ioport << 2;

	uart->port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	uart->port.uartclk = 115200 * 16;
	uart->port.irq = pci_irq_vector(pdev, vuart_msi_index[idx]);
	uart->port.dev = dev;
	uart->port.iotype = UPIO_MEM32;
	uart->port.iobase = 0;
	uart->port.mapbase = pci_bmc_dev->message_bar_base + vuart_ioport;
	uart->port.membase = 0;
	uart->port.type = PORT_16550A;
	uart->port.flags |= (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE);
	uart->port.regshift = 2;

	ret = serial8250_register_8250_port(&pci_bmc_dev->uart[idx]);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Can't setup PCIe VUART%d\n", idx);
		return ret;
	}

	pci_bmc_dev->uart_line[idx] = ret;

	return 0;
}

static void aspeed_pci_host_bmc_device_release_vuart(struct pci_dev *pdev, int idx)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);

	if (pci_bmc_dev->uart_line[idx] >= 0)
		serial8250_unregister_port(pci_bmc_dev->uart_line[idx]);
}

static int aspeed_pci_host_setup(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	int rc = 0;

	pci_bmc_dev->message_bar_base = pci_resource_start(pdev, 1);

	if (pdev->revision == 0x27) {
		pr_err("AST2700 detected but not supported");
		return -ENODEV;
	}

	rc = aspeed_pci_bmc_device_setup_vuart(pdev, 0);
	if (rc)
		return rc;

	rc = aspeed_pci_bmc_device_setup_vuart(pdev, 1);
	if (rc)
		goto out_free_VUART0;

	return 0;

out_free_VUART0:
	aspeed_pci_host_bmc_device_release_vuart(pdev, 0);

	return rc;
}

static int aspeed_pci_host_bmc_device_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev;
	int rc = 0;

	pci_bmc_dev = devm_kzalloc(&pdev->dev, sizeof(*pci_bmc_dev), GFP_KERNEL);
	if (!pci_bmc_dev)
		return -ENOMEM;

	rc = pci_enable_device(pdev);
	if (rc) {
		dev_err(&pdev->dev, "pci_enable_device() returned error %d\n", rc);
		return rc;
	}

	pci_set_master(pdev);
	pci_set_drvdata(pdev, pci_bmc_dev);

	rc = pci_alloc_irq_vectors(pdev, BMC_MULTI_MSI, BMC_MULTI_MSI, PCI_IRQ_INTX | PCI_IRQ_MSI);
	if (rc < 0) {
		dev_err(&pdev->dev, "aspeed_pci_setup_irq_resource() returned error %d\n", rc);
		goto disable_device;
	}

	/* Setup BMC PCI device */
	rc = aspeed_pci_host_setup(pdev);
	if (rc) {
		dev_err(&pdev->dev, "ASPEED PCIe Host device returned error %d\n", rc);
		goto free_irq;
	}

	return 0;

free_irq:
	pci_free_irq_vectors(pdev);
disable_device:
	pci_disable_device(pdev);
	return rc;
}

static void aspeed_pci_host_bmc_device_remove(struct pci_dev *pdev)
{
	aspeed_pci_host_bmc_device_release_vuart(pdev, 0);
	aspeed_pci_host_bmc_device_release_vuart(pdev, 1);

	pci_free_irq_vectors(pdev);
	pci_disable_device(pdev);
}

static struct pci_device_id aspeed_host_bmc_dev_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ASPEED, PCI_BMC_DEVICE_ID),
		.class = 0xFF0000, .class_mask = 0xFFFF00,
		.driver_data = ASPEED },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, aspeed_host_bmc_dev_pci_ids);

static struct pci_driver aspeed_host_bmc_dev_driver = {
	.name		= DRIVER_NAME,
	.id_table	= aspeed_host_bmc_dev_pci_ids,
	.probe		= aspeed_pci_host_bmc_device_probe,
	.remove		= aspeed_pci_host_bmc_device_remove,
};

module_driver(aspeed_host_bmc_dev_driver, pci_register_driver, pci_unregister_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED Host BMC DEVICE Driver");
MODULE_LICENSE("GPL");
