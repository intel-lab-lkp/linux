// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <drm/drm_file.h>
#include "qda_drv.h"
#include "qda_memory_manager.h"

static void cleanup_all_memory_devices(struct qda_memory_manager *mem_mgr)
{
	unsigned long index;
	void *entry;

	pr_debug("qda: Starting cleanup of all memory devices\n");

	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		pr_debug("qda: Cleaning up device id=%lu\n", index);

		xa_erase(&mem_mgr->device_xa, index);
		kfree(iommu_dev);
	}

	pr_debug("qda: Completed cleanup of all memory devices\n");
}

static int allocate_device_id(struct qda_memory_manager *mem_mgr,
			      struct qda_iommu_device *iommu_dev, u32 *id)
{
	int ret;

	ret = xa_alloc(&mem_mgr->device_xa, id, iommu_dev,
		       xa_limit_31b, GFP_KERNEL);
	if (ret) {
		dev_err(iommu_dev->dev, "Failed to allocate XArray ID: %d\n", ret);
		return ret;
	}

	dev_dbg(iommu_dev->dev, "Allocated device id=%u\n", *id);
	return 0;
}

/**
 * qda_memory_manager_register_device() - Register an IOMMU device
 * @mem_mgr: Pointer to memory manager
 * @iommu_dev: Pointer to IOMMU device to register
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_register_device(struct qda_memory_manager *mem_mgr,
				       struct qda_iommu_device *iommu_dev)
{
	int ret;
	u32 id;

	ret = allocate_device_id(mem_mgr, iommu_dev, &id);
	if (ret) {
		dev_err(iommu_dev->dev,
			"Failed to allocate device ID: %d (sid=%u)\n",
			ret, iommu_dev->sid);
		return ret;
	}

	iommu_dev->id = id;

	dev_dbg(iommu_dev->dev, "Registered device id=%u (sid=%u)\n", id, iommu_dev->sid);

	return 0;
}

/**
 * qda_memory_manager_unregister_device() - Unregister an IOMMU device
 * @mem_mgr: Pointer to memory manager
 * @iommu_dev: Pointer to IOMMU device to unregister
 */
void qda_memory_manager_unregister_device(struct qda_memory_manager *mem_mgr,
					  struct qda_iommu_device *iommu_dev)
{
	xa_erase(&mem_mgr->device_xa, iommu_dev->id);
	kfree(iommu_dev);
}

/**
 * qda_memory_manager_init() - Initialize the memory manager
 * @mem_mgr: Pointer to memory manager structure to initialize
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_init(struct qda_memory_manager *mem_mgr)
{
	pr_debug("qda: Initializing memory manager\n");

	xa_init_flags(&mem_mgr->device_xa, XA_FLAGS_ALLOC);

	pr_debug("qda: Memory manager initialized successfully\n");
	return 0;
}

/**
 * qda_memory_manager_exit() - Clean up the memory manager
 * @mem_mgr: Pointer to memory manager structure to clean up
 */
void qda_memory_manager_exit(struct qda_memory_manager *mem_mgr)
{
	cleanup_all_memory_devices(mem_mgr);
	pr_debug("qda: Memory manager exited\n");
}
