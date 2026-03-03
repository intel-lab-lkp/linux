// SPDX-License-Identifier: GPL-2.0-only
/*
 *  This module handles PCI endpoint functions exposed by Realtek
 *  management controllers (e.g. RTL8111x series). It manages device
 *  probing for virtual devices.
 *
 *  Copyright(c) 2026 Realtek Semiconductor Corp.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/init.h>

#define PCI_DEVICE_ID_REALTEK_PTOU      0x8164
#define PCI_DEVICE_ID_REALTEK_COM1      0x816a
#define PCI_DEVICE_ID_REALTEK_COM2      0x816b
#define PCI_DEVICE_ID_REALTEK_IPMI      0x816c
#define PCI_DEVICE_ID_REALTEK_BMC       0x816e
#define PCI_DEVICE_ID_REALTEK_PCIBR     0x9151

static struct pci_device_id rtl_pci_tbl[] = {
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_PTOU), },
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_COM1), },
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_COM2), },
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_IPMI), },
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_BMC), },
	{ PCI_VDEVICE(REALTEK, PCI_DEVICE_ID_REALTEK_PCIBR), .class_mask = 0xff00 },
	{ }
};

MODULE_DEVICE_TABLE(pci, rtl_pci_tbl);

static int rtl_probe(struct pci_dev *pdev,
			 const struct pci_device_id *ent)
{
	int rc;

	/* enable device (incl. PCI PM wakeup and hotplug setup) */
	rc = pcim_enable_device(pdev);
	if (rc < 0)
		return dev_err_probe(&pdev->dev, rc, "enable failure\n");

	dev_info(&pdev->dev, "enable device\n");

	return rc;
}

static void rtl_remove(struct pci_dev *pdev) {}

static int rtl_pm_suspend(struct device *device)
{
	return 0;
}

static int rtl_pm_resume(struct device *device)
{
	return 0;
}

static const struct dev_pm_ops rtl_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rtl_pm_suspend, rtl_pm_resume)
};

static struct pci_driver rtl_pci_driver = {
	.name		= "rtl_pci",
	.id_table	= rtl_pci_tbl,
	.probe		= rtl_probe,
	.remove		= rtl_remove,
#ifdef CONFIG_PM
	.driver.pm  = pm_ptr(&rtl_pm_ops),
#endif
};

module_pci_driver(rtl_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RealTek pci driver");
