/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_PCI_H_
#define _XE_PCI_H_

struct pci_dev;
struct pci_device_id;

int xe_register_pci_driver(void);
void xe_unregister_pci_driver(void);
void xe_load_pci_state(struct pci_dev *pdev);
int xe_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent);
#endif
