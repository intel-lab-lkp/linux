// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) ASPEED Technology Inc.

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/serial_8250.h>
#include <linux/mfd/core.h>

#define ASPEED_BMC_MULTI_MSI		32
#define ASPEED_BMC_PCI_DEVICE_ID	0x2402
#define ASPEED_BMC_REVISION_AST2700	0x27
#define ASPEED_BMC_VUART		2

#define DRIVER_NAME "ast2600-pci-core"

static const unsigned int vuart_msi_index[ASPEED_BMC_VUART] = { 16, 17 };
static const u16 vuart_port_addr[ASPEED_BMC_VUART] = { 0x3f8, 0x2f8 };

struct aspeed_pci_bmc_dev {
	struct plat_serial8250_port uart[ASPEED_BMC_VUART + 1];
	struct mfd_cell cell;
};

static int aspeed_pci_bmc_device_setup_vuart(struct pci_dev *pdev,
					     struct aspeed_pci_bmc_dev *pci_bmc_dev)
{
	resource_size_t bar = pci_resource_start(pdev, 1);
	struct plat_serial8250_port *port;
	u16 vuart_ioport;
	unsigned int i;

	for (i = 0; i < ASPEED_BMC_VUART; i++) {
		port = &pci_bmc_dev->uart[i];

		/* ASPEED BMC device shift addresses by 2 to the left */
		vuart_ioport = vuart_port_addr[i] << 2;

		port->mapbase = bar + vuart_ioport;
		port->uartclk = 115200 * 16;
		port->irq = pci_irq_vector(pdev, vuart_msi_index[i]);
		port->iotype = UPIO_MEM32;
		port->type = PORT_16550A;
		port->flags |= (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE);
		port->regshift = 2;
	}

	pci_bmc_dev->cell = (struct mfd_cell) {
		.name		= "serial8250",
		.platform_data	= pci_bmc_dev->uart,
		.pdata_size	= sizeof(pci_bmc_dev->uart),
	};

	return 0;
}

static void aspeed_bmc_pci_free_irqs(void *pdev)
{
	pci_free_irq_vectors(pdev);
}

static int aspeed_pci_host_bmc_device_probe(struct pci_dev *pdev,
					    const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct aspeed_pci_bmc_dev *pci_bmc_dev;
	int rc = 0;

	if (pdev->revision == ASPEED_BMC_REVISION_AST2700)
		return dev_err_probe(dev, -ENODEV, "AST2700 detected but not supported\n");

	pci_bmc_dev = devm_kzalloc(dev, sizeof(*pci_bmc_dev), GFP_KERNEL);
	if (!pci_bmc_dev)
		return -ENOMEM;

	rc = pcim_enable_device(pdev);
	if (rc)
		return dev_err_probe(dev, rc, "failed to enable device\n");

	pci_set_master(pdev);

	rc = pci_alloc_irq_vectors(pdev, ASPEED_BMC_MULTI_MSI, ASPEED_BMC_MULTI_MSI, PCI_IRQ_MSI);
	if (rc < 0)
		return dev_err_probe(dev, rc, "failed to allocate %d MSI vectors\n",
				     ASPEED_BMC_MULTI_MSI);

	rc = devm_add_action_or_reset(dev, aspeed_bmc_pci_free_irqs, pdev);
	if (rc)
		return rc;

	aspeed_pci_bmc_device_setup_vuart(pdev, pci_bmc_dev);

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    &pci_bmc_dev->cell, 1, &pdev->resource[1],
				    0, NULL);
}

static struct pci_device_id aspeed_bmc_dev_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ASPEED, ASPEED_BMC_PCI_DEVICE_ID),
		.class = PCI_CLASS_OTHERS << 16,
		.class_mask = 0xFFFF00
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, aspeed_bmc_dev_pci_ids);

static struct pci_driver aspeed_host_bmc_dev_driver = {
	.name		= DRIVER_NAME,
	.id_table	= aspeed_bmc_dev_pci_ids,
	.probe		= aspeed_pci_host_bmc_device_probe,
};

module_pci_driver(aspeed_host_bmc_dev_driver);

MODULE_AUTHOR("Grégoire Layet <gregoire.layet@9elements.com>");
MODULE_DESCRIPTION("Host-side driver for the ASPEED BMC PCIe device");
MODULE_LICENSE("GPL");
