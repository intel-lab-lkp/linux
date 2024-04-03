// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include <linux/pci.h>
#include <linux/types.h>

#include "fbnic.h"

void fbnic_free_irqs(struct fbnic_dev *fbd)
{
	struct pci_dev *pdev = to_pci_dev(fbd->dev);

	fbd->num_irqs = 0;

	pci_disable_msix(pdev);

	kfree(fbd->msix_entries);
	fbd->msix_entries = NULL;
}

int fbnic_alloc_irqs(struct fbnic_dev *fbd)
{
	unsigned int wanted_irqs = FBNIC_NON_NAPI_VECTORS;
	struct pci_dev *pdev = to_pci_dev(fbd->dev);
	struct msix_entry *msix_entries;
	int i, num_irqs;

	msix_entries = kcalloc(wanted_irqs, sizeof(*msix_entries), GFP_KERNEL);
	if (!msix_entries)
		return -ENOMEM;

	for (i = 0; i < wanted_irqs; i++)
		msix_entries[i].entry = i;

	num_irqs = pci_enable_msix_range(pdev, msix_entries,
					 FBNIC_NON_NAPI_VECTORS + 1,
					 wanted_irqs);
	if (num_irqs < 0) {
		dev_err(fbd->dev, "Failed to allocate MSI-X entries\n");
		kfree(msix_entries);
		return num_irqs;
	}

	if (num_irqs < wanted_irqs)
		dev_warn(fbd->dev, "Allocated %d IRQs, expected %d\n",
			 num_irqs, wanted_irqs);

	fbd->msix_entries = msix_entries;
	fbd->num_irqs = num_irqs;

	return 0;
}
