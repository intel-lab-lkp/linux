/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VERSAL_PCI_COMM_CHAN_H
#define __VERSAL_PCI_COMM_CHAN_H

struct comm_chan_device *versal_pci_comm_chan_init(struct versal_pci_device *vdev);
void versal_pci_comm_chan_fini(struct comm_chan_device *ccdev);

#endif	/* __VERSAL_PCI_COMM_CHAN_H */
