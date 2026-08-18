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
#define ASPEED_BMC_NR_VUART		2

#define DRIVER_NAME "ast2600-pci-core"

static const unsigned int vuart_msi_index[ASPEED_BMC_NR_VUART] = { 16, 17 };
static const u16 vuart_port_addr[ASPEED_BMC_NR_VUART] = { 0x3f8, 0x2f8 };

static struct plat_serial8250_port aspeed_uart_port[ASPEED_BMC_NR_VUART + 1] = {
	{
		.uartclk = 115200 * 16,
		.iotype = UPIO_MEM32,
		.type = PORT_16550A,
		.flags = (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE),
		.regshift = 2
	},
	{
		.uartclk = 115200 * 16,
		.iotype = UPIO_MEM32,
		.type = PORT_16550A,
		.flags = (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE),
		.regshift = 2
	},
	{ 0 }
};

static const struct mfd_cell aspeed_bmc_cell = {
	.name		= "serial8250",
	.platform_data	= aspeed_uart_port,
	.pdata_size	= sizeof(aspeed_uart_port),
};

static int aspeed_pci_bmc_device_setup_vuart(struct pci_dev *pdev)
{
	resource_size_t bar = pci_resource_start(pdev, 1);
	unsigned int i;

	for (i = 0; i < ASPEED_BMC_NR_VUART; i++) {
		aspeed_uart_port[i].mapbase = bar + (vuart_port_addr[i] << 2);
		aspeed_uart_port[i].irq = pci_irq_vector(pdev, vuart_msi_index[i]);
	}

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
	int rc = 0;

	if (pdev->revision == ASPEED_BMC_REVISION_AST2700)
		return dev_err_probe(dev, -ENODEV, "AST2700 detected but not supported\n");

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

	aspeed_pci_bmc_device_setup_vuart(pdev);

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    &aspeed_bmc_cell, 1, &pdev->resource[1],
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
