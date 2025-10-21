/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_VFIO_H_
#define _XE_SRIOV_VFIO_H_

#include <linux/types.h>

struct pci_dev;

bool xe_sriov_vfio_migration_supported(struct pci_dev *pdev);
int xe_sriov_vfio_wait_flr_done(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_stop(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_run(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_stop_copy_enter(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_stop_copy_exit(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_resume_enter(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_resume_exit(struct pci_dev *pdev, unsigned int vfid);
int xe_sriov_vfio_error(struct pci_dev *pdev, unsigned int vfid);
ssize_t xe_sriov_vfio_data_read(struct pci_dev *pdev, unsigned int vfid,
				char __user *buf, size_t len);
ssize_t xe_sriov_vfio_data_write(struct pci_dev *pdev, unsigned int vfid,
				 const char __user *buf, size_t len);
ssize_t xe_sriov_vfio_stop_copy_size(struct pci_dev *pdev, unsigned int vfid);

#endif
