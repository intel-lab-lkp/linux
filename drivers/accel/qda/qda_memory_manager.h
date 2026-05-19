/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_MEMORY_MANAGER_H__
#define __QDA_MEMORY_MANAGER_H__

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <drm/drm_file.h>

/* Forward declarations */
struct qda_dev;
struct qda_gem_obj;

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
	/** @assigned_file_priv: DRM file private data for the assigned process */
	struct drm_file *assigned_file_priv;
	/** @id: Unique identifier assigned by the memory manager XArray */
	u32 id;
	/** @sid: Stream ID for IOMMU transactions */
	u32 sid;
	/** @assigned_pid: Process ID of the process assigned to this device */
	pid_t assigned_pid;
	/** @refcount: Reference counter for device */
	refcount_t refcount;
	/** @lock: Spinlock protecting concurrent access to device */
	spinlock_t lock;
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
	/** @process_assignment_lock: Mutex protecting process-to-device assignments */
	struct mutex process_assignment_lock;
};

int qda_memory_manager_init(struct qda_memory_manager *mem_mgr);
void qda_memory_manager_exit(struct qda_memory_manager *mem_mgr);

int qda_memory_manager_register_device(struct qda_memory_manager *mem_mgr,
				       struct qda_iommu_device *iommu_dev);
void qda_memory_manager_unregister_device(struct qda_memory_manager *mem_mgr,
					  struct qda_iommu_device *iommu_dev);

int qda_memory_manager_assign_device(struct qda_memory_manager *mem_mgr,
				     struct drm_file *file_priv);

int qda_memory_manager_alloc(struct qda_memory_manager *mem_mgr,
			     struct qda_gem_obj *gem_obj,
			     struct drm_file *file_priv);
void qda_memory_manager_free(struct qda_memory_manager *mem_mgr,
			     struct qda_gem_obj *gem_obj);

#endif /* __QDA_MEMORY_MANAGER_H__ */
