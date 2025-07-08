// SPDX-License-Identifier: GPL-2.0

#include <linux/pci.h>

void rust_helper_pci_set_drvdata(struct pci_dev *pdev, void *data)
{
	pci_set_drvdata(pdev, data);
}

void *rust_helper_pci_get_drvdata(struct pci_dev *pdev)
{
	return pci_get_drvdata(pdev);
}

u16 rust_helper_pci_dev_id(struct pci_dev *dev)
{
	return PCI_DEVID(dev->bus->number, dev->devfn);
}

resource_size_t rust_helper_pci_resource_start(struct pci_dev *pdev, int bar)
{
	return pci_resource_start(pdev, bar);
}

resource_size_t rust_helper_pci_resource_len(struct pci_dev *pdev, int bar)
{
	return pci_resource_len(pdev, bar);
}

bool rust_helper_dev_is_pci(const struct device *dev)
{
	return dev_is_pci(dev);
}
