// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <drm/drm_file.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"
#include "qda_memory_dma.h"

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
	iommu_dev->assigned_pid = 0;
	iommu_dev->assigned_file_priv = NULL;
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

static struct qda_iommu_device *find_device_for_pid(struct qda_memory_manager *mem_mgr,
						    pid_t pid)
{
	unsigned long index;
	void *entry;
	struct qda_iommu_device *found_dev = NULL;
	unsigned long flags;

	xa_lock(&mem_mgr->device_xa);
	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		spin_lock_irqsave(&iommu_dev->lock, flags);
		if (iommu_dev->assigned_pid == pid) {
			found_dev = iommu_dev;
			refcount_inc(&found_dev->refcount);
			qda_dbg(NULL, "Reusing device id=%u for PID=%d (refcount=%u)\n",
				found_dev->id, pid, refcount_read(&found_dev->refcount));
			spin_unlock_irqrestore(&iommu_dev->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&iommu_dev->lock, flags);
	}
	xa_unlock(&mem_mgr->device_xa);

	return found_dev;
}

static struct qda_iommu_device *assign_available_device_to_pid(struct qda_memory_manager *mem_mgr,
							       pid_t pid,
							       struct drm_file *file_priv)
{
	unsigned long index;
	void *entry;
	struct qda_iommu_device *selected_dev = NULL;
	unsigned long flags;

	xa_lock(&mem_mgr->device_xa);
	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		spin_lock_irqsave(&iommu_dev->lock, flags);
		if (iommu_dev->assigned_pid == 0) {
			iommu_dev->assigned_pid = pid;
			iommu_dev->assigned_file_priv = file_priv;
			selected_dev = iommu_dev;
			refcount_set(&selected_dev->refcount, 1);
			qda_dbg(NULL, "Assigned device id=%u to PID=%d\n",
				selected_dev->id, pid);
			spin_unlock_irqrestore(&iommu_dev->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&iommu_dev->lock, flags);
	}
	xa_unlock(&mem_mgr->device_xa);

	return selected_dev;
}

static struct qda_iommu_device *get_process_iommu_device(struct qda_memory_manager *mem_mgr,
							 struct drm_file *file_priv)
{
	struct qda_file_priv *qda_priv;

	if (!file_priv || !file_priv->driver_priv)
		return NULL;

	qda_priv = (struct qda_file_priv *)file_priv->driver_priv;
	return qda_priv->assigned_iommu_dev;
}

static int qda_memory_manager_assign_device(struct qda_memory_manager *mem_mgr,
					    struct drm_file *file_priv)
{
	struct qda_file_priv *qda_priv;
	struct qda_iommu_device *selected_dev = NULL;
	int ret = 0;
	pid_t current_pid;

	if (!file_priv || !file_priv->driver_priv) {
		qda_err(NULL, "Invalid file_priv or driver_priv\n");
		return -EINVAL;
	}

	qda_priv = (struct qda_file_priv *)file_priv->driver_priv;
	current_pid = qda_priv->pid;

	mutex_lock(&mem_mgr->process_assignment_lock);

	if (qda_priv->assigned_iommu_dev) {
		qda_dbg(NULL, "PID=%d already has device id=%u assigned\n",
			current_pid, qda_priv->assigned_iommu_dev->id);
		ret = 0;
		goto unlock_and_return;
	}

	selected_dev = find_device_for_pid(mem_mgr, current_pid);

	if (selected_dev) {
		qda_priv->assigned_iommu_dev = selected_dev;
		goto unlock_and_return;
	}

	selected_dev = assign_available_device_to_pid(mem_mgr, current_pid, file_priv);

	if (!selected_dev) {
		qda_err(NULL, "No available device for PID=%d\n", current_pid);
		ret = -ENOMEM;
		goto unlock_and_return;
	}

	qda_priv->assigned_iommu_dev = selected_dev;

unlock_and_return:
	mutex_unlock(&mem_mgr->process_assignment_lock);
	return ret;
}

static struct qda_iommu_device *get_or_assign_iommu_device(struct qda_memory_manager *mem_mgr,
							   struct drm_file *file_priv,
							   size_t size)
{
	struct qda_iommu_device *iommu_dev;
	int ret;

	iommu_dev = get_process_iommu_device(mem_mgr, file_priv);
	if (iommu_dev)
		return iommu_dev;

	ret = qda_memory_manager_assign_device(mem_mgr, file_priv);
	if (ret)
		return NULL;

	iommu_dev = get_process_iommu_device(mem_mgr, file_priv);
	if (iommu_dev)
		return iommu_dev;

	return NULL;
}

int qda_memory_manager_alloc(struct qda_memory_manager *mem_mgr, struct qda_gem_obj *gem_obj,
			     struct drm_file *file_priv)
{
	struct qda_iommu_device *selected_dev;
	size_t size;
	int ret;

	if (!mem_mgr || !gem_obj || !file_priv) {
		qda_err(NULL, "Invalid parameters for memory allocation\n");
		return -EINVAL;
	}

	size = gem_obj->size;
	if (size == 0) {
		qda_err(NULL, "Invalid allocation size: 0\n");
		return -EINVAL;
	}

	selected_dev = get_or_assign_iommu_device(mem_mgr, file_priv, size);

	if (!selected_dev) {
		qda_err(NULL, "Failed to get/assign device for allocation (size=%zu)\n", size);
		return -ENOMEM;
	}

	ret = qda_dma_alloc(selected_dev, gem_obj, size);

	if (ret) {
		qda_err(NULL, "Allocation failed: size=%zu, device_id=%u, ret=%d\n",
			size, selected_dev->id, ret);
		return ret;
	}

	qda_dbg(NULL, "Successfully allocated: size=%zu, device_id=%u, dma_addr=0x%llx\n",
		size, selected_dev->id, gem_obj->dma_addr);
	return 0;
}

void qda_memory_manager_free(struct qda_memory_manager *mem_mgr, struct qda_gem_obj *gem_obj)
{
	if (!gem_obj || !gem_obj->iommu_dev) {
		qda_dbg(NULL, "Invalid gem_obj or iommu_dev for free\n");
		return;
	}

	qda_dma_free(gem_obj);
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
	mutex_init(&mem_mgr->process_assignment_lock);
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
