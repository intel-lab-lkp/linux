/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, INTEL CORPORATION. All rights reserved
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved
 */

#ifndef NVME_VFIO_PCI_H
#define NVME_VFIO_PCI_H

#include <linux/kernel.h>
#include <linux/vfio_pci_core.h>
#include <linux/nvme.h>

struct nvmevf_migration_file {
	struct file *filp;
	struct mutex lock;
	bool disabled;
	u8 *vf_data;
	size_t total_length;
};

struct nvmevf_pci_core_device {
	struct vfio_pci_core_device core_device;
	int vf_id;
	u8 migrate_cap:1;
	u8 deferred_reset:1;
	/* protect migration state */
	struct mutex state_mutex;
	enum vfio_device_mig_state mig_state;
	/* protect the reset_done flow */
	spinlock_t reset_lock;
	struct nvmevf_migration_file *resuming_migf;
	struct nvmevf_migration_file *saving_migf;
};

#endif /* NVME_VFIO_PCI_H */
