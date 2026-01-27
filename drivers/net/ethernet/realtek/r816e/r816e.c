// SPDX-License-Identifier: GPL-2.0-only
/*
 *  r816e is the Linux device driver released for Realtek RTL8116AF nic
 *  with PCI-Express interface, which is used for power management.
 *
 *  Copyright(c) 2026 Realtek Semiconductor Corp.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/init.h>

static struct pci_device_id rtl816e_pci_tbl[] = {
	{ PCI_VDEVICE(REALTEK, 0x816e), },
	{ 0, },
};

MODULE_DEVICE_TABLE(pci, rtl816e_pci_tbl);

static int rtl816e_probe(struct pci_dev *pdev,
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

static void rtl816e_remove(struct pci_dev *pdev) {}

static int rtl816e_pm_suspend(struct device *device)
{
	return 0;
}

static int rtl816e_pm_resume(struct device *device)
{
	return 0;
}

static const struct dev_pm_ops rtl816e_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(rtl816e_pm_suspend, rtl816e_pm_resume)
};

static struct pci_driver rtl816e_pci_driver = {
	.name		= "r816e",
	.id_table	= rtl816e_pci_tbl,
	.probe		= rtl816e_probe,
	.remove		= rtl816e_remove,
#ifdef CONFIG_PM
	.driver.pm  = pm_ptr(&rtl816e_pm_ops),
#endif
};

static int __init rtl816e_init_module(void)
{
	return pci_register_driver(&rtl816e_pci_driver);
}

static void __exit rtl816e_cleanup_module(void)
{
	pci_unregister_driver(&rtl816e_pci_driver);
}

module_init(rtl816e_init_module);
module_exit(rtl816e_cleanup_module);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RealTek RTL816E driver");
