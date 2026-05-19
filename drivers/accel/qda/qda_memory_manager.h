/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_MEMORY_MANAGER_H__
#define __QDA_MEMORY_MANAGER_H__

#include <linux/device.h>
#include <linux/xarray.h>
#include "qda_drv.h"

/**
 * struct qda_iommu_device - IOMMU device instance for memory management
 *
 * Represents a single IOMMU-enabled device managed by the memory manager.
 * Each device can be assigned to a specific process session.
 */
struct qda_iommu_device {
	/** @dev: Pointer to the underlying device */
	struct device *dev;
	/** @qdev: Back-pointer to the parent QDA device */
	struct qda_dev *qdev;
	/** @id: Unique identifier assigned by the memory manager XArray */
	u32 id;
	/** @sid: Stream ID for IOMMU transactions */
	u32 sid;
};

/**
 * struct qda_memory_manager - Central memory management coordinator
 *
 * Coordinates memory management across multiple IOMMU devices. Maintains
 * a registry of devices using an XArray for O(1) lookup by ID.
 */
struct qda_memory_manager {
	/** @device_xa: XArray storing all registered IOMMU devices */
	struct xarray device_xa;
};

int qda_memory_manager_init(struct qda_memory_manager *mem_mgr);
void qda_memory_manager_exit(struct qda_memory_manager *mem_mgr);

int qda_memory_manager_register_device(struct qda_memory_manager *mem_mgr,
				       struct qda_iommu_device *iommu_dev);
void qda_memory_manager_unregister_device(struct qda_memory_manager *mem_mgr,
					  struct qda_iommu_device *iommu_dev);

#endif /* __QDA_MEMORY_MANAGER_H__ */
