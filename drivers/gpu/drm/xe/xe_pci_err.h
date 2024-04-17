/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_PCI_ERR_H_
#define _XE_PCI_ERR_H_

struct pci_dev;

void xe_pci_reset_prepare(struct pci_dev *pdev);
void xe_pci_reset_done(struct pci_dev *pdev);
#endif
