/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _QDA_MEMORY_MANAGER_H
#define _QDA_MEMORY_MANAGER_H

#include <linux/device.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

/**
 * struct qda_iommu_device - IOMMU device instance for memory management
 *
 * This structure represents a single IOMMU-enabled device managed by the
 * memory manager. Each device can be assigned to a specific process.
 */
struct qda_iommu_device {
	/* Unique identifier for this IOMMU device */
	u32 id;
	/* Pointer to the underlying device */
	struct device *dev;
	/* Name for the device */
	char name[32];
	/* Spinlock protecting concurrent access to device */
	spinlock_t lock;
	/* Reference counter for device */
	refcount_t refcount;
	/* Work structure for deferred device removal */
	struct work_struct remove_work;
	/* Stream ID for IOMMU transactions */
	u32 sid;
	/* Pointer to parent memory manager */
	struct qda_memory_manager *manager;
};

/**
 * struct qda_memory_manager - Central memory management coordinator
 *
 * This is the top-level structure coordinating memory management across
 * multiple IOMMU devices. It maintains a registry of devices and backends,
 * and ensures thread-safe access to shared resources.
 */
struct qda_memory_manager {
	/* XArray storing all registered IOMMU devices */
	struct xarray device_xa;
	/* Atomic counter for generating unique device IDs */
	atomic_t next_id;
	/* Workqueue for asynchronous device operations */
	struct workqueue_struct *wq;
};

/**
 * qda_memory_manager_init() - Initialize the memory manager
 * @mem_mgr: Pointer to memory manager structure to initialize
 *
 * Initializes the memory manager's internal data structures including
 * the device registry, workqueue, and synchronization primitives.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_init(struct qda_memory_manager *mem_mgr);

/**
 * qda_memory_manager_exit() - Clean up the memory manager
 * @mem_mgr: Pointer to memory manager structure to clean up
 *
 * Releases all resources associated with the memory manager, including
 * unregistering all devices and destroying the workqueue.
 */
void qda_memory_manager_exit(struct qda_memory_manager *mem_mgr);

/**
 * qda_memory_manager_register_device() - Register an IOMMU device
 * @mem_mgr: Pointer to memory manager
 * @iommu_dev: Pointer to IOMMU device to register
 *
 * Adds a new IOMMU device to the memory manager's registry and initializes
 * its memory backend. The device becomes available for memory allocation
 * operations.
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_register_device(struct qda_memory_manager *mem_mgr,
				       struct qda_iommu_device *iommu_dev);

/**
 * qda_memory_manager_unregister_device() - Unregister an IOMMU device
 * @mem_mgr: Pointer to memory manager
 * @iommu_dev: Pointer to IOMMU device to unregister
 *
 * Removes an IOMMU device from the memory manager's registry and cleans up
 * its associated resources. Any remaining memory allocations are freed.
 */
void qda_memory_manager_unregister_device(struct qda_memory_manager *mem_mgr,
					  struct qda_iommu_device *iommu_dev);

#endif /* _QDA_MEMORY_MANAGER_H */
