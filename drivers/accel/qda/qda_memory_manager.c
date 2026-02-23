// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include "qda_drv.h"
#include "qda_memory_manager.h"

static void cleanup_all_memory_devices(struct qda_memory_manager *mem_mgr)
{
	unsigned long index;
	void *entry;

	qda_dbg(NULL, "Starting cleanup of all memory devices\n");

	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		qda_dbg(NULL, "Cleaning up device id=%lu\n", index);

		xa_erase(&mem_mgr->device_xa, index);
		kfree(iommu_dev);
	}

	qda_dbg(NULL, "Completed cleanup of all memory devices\n");
}

static void qda_memory_manager_remove_work(struct work_struct *work)
{
	struct qda_iommu_device *iommu_dev =
		container_of(work, struct qda_iommu_device, remove_work);
	struct qda_memory_manager *mem_mgr = iommu_dev->manager;

	qda_dbg(NULL, "Remove work started for device id=%u\n", iommu_dev->id);

	if (!mem_mgr) {
		qda_dbg(NULL, "No manager for device id=%u\n", iommu_dev->id);
		kfree(iommu_dev);
		return;
	}

	xa_erase(&mem_mgr->device_xa, iommu_dev->id);

	qda_dbg(NULL, "Device id=%u removed successfully\n", iommu_dev->id);
	kfree(iommu_dev);
}

static void init_iommu_device_fields(struct qda_iommu_device *iommu_dev,
				     struct qda_memory_manager *mem_mgr)
{
	iommu_dev->manager = mem_mgr;
	spin_lock_init(&iommu_dev->lock);
	refcount_set(&iommu_dev->refcount, 0);
	INIT_WORK(&iommu_dev->remove_work, qda_memory_manager_remove_work);
}

static int allocate_device_id(struct qda_memory_manager *mem_mgr,
			      struct qda_iommu_device *iommu_dev, u32 *id)
{
	int ret;

	ret = xa_alloc(&mem_mgr->device_xa, id, iommu_dev,
		       xa_limit_31b, GFP_KERNEL);
	if (ret) {
		qda_dbg(NULL, "xa_alloc failed, using atomic counter\n");
		*id = atomic_inc_return(&mem_mgr->next_id);
		ret = xa_insert(&mem_mgr->device_xa, *id, iommu_dev, GFP_KERNEL);
		if (ret) {
			qda_err(NULL, "Failed to insert device with id=%u: %d\n", *id, ret);
			return ret;
		}
	}

	qda_dbg(NULL, "Allocated device id=%u\n", *id);
	return ret;
}

int qda_memory_manager_register_device(struct qda_memory_manager *mem_mgr,
				       struct qda_iommu_device *iommu_dev)
{
	int ret;
	u32 id;

	if (!mem_mgr || !iommu_dev || !iommu_dev->dev) {
		qda_err(NULL, "Invalid parameters for device registration\n");
		return -EINVAL;
	}

	init_iommu_device_fields(iommu_dev, mem_mgr);

	ret = allocate_device_id(mem_mgr, iommu_dev, &id);
	if (ret) {
		qda_err(NULL, "Failed to allocate device ID: %d (sid=%u)\n", ret, iommu_dev->sid);
		return ret;
	}

	iommu_dev->id = id;

	qda_dbg(NULL, "Registered device id=%u (sid=%u)\n", id, iommu_dev->sid);

	return 0;
}

void qda_memory_manager_unregister_device(struct qda_memory_manager *mem_mgr,
					  struct qda_iommu_device *iommu_dev)
{
	if (!mem_mgr || !iommu_dev) {
		qda_err(NULL, "Attempted to unregister invalid device/manager\n");
		return;
	}

	qda_dbg(NULL, "Unregistering device id=%u (refcount=%u)\n", iommu_dev->id,
		refcount_read(&iommu_dev->refcount));

	if (refcount_read(&iommu_dev->refcount) == 0) {
		xa_erase(&mem_mgr->device_xa, iommu_dev->id);
		kfree(iommu_dev);
		return;
	}

	if (refcount_dec_and_test(&iommu_dev->refcount)) {
		qda_info(NULL, "Device id=%u refcount reached zero, queuing removal\n",
			 iommu_dev->id);
		queue_work(mem_mgr->wq, &iommu_dev->remove_work);
	}
}

int qda_memory_manager_init(struct qda_memory_manager *mem_mgr)
{
	qda_dbg(NULL, "Initializing memory manager\n");

	xa_init_flags(&mem_mgr->device_xa, XA_FLAGS_ALLOC);
	atomic_set(&mem_mgr->next_id, 0);
	mem_mgr->wq = create_workqueue("memory_manager_wq");
	if (!mem_mgr->wq) {
		qda_err(NULL, "Failed to create memory manager workqueue\n");
		return -ENOMEM;
	}

	qda_dbg(NULL, "QDA: Memory manager initialized successfully\n");
	return 0;
}

void qda_memory_manager_exit(struct qda_memory_manager *mem_mgr)
{
	cleanup_all_memory_devices(mem_mgr);
	destroy_workqueue(mem_mgr->wq);
	qda_dbg(NULL, "QDA: Memory manager exited\n");
}
