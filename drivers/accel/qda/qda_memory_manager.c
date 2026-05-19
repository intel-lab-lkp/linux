// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"
#include "qda_memory_dma.h"

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

static void init_iommu_device_fields(struct qda_iommu_device *iommu_dev)
{
	spin_lock_init(&iommu_dev->lock);
	refcount_set(&iommu_dev->refcount, 0);
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
		dev_err(iommu_dev->dev, "Failed to allocate XArray ID: %d\n", ret);
		return ret;
	}

	dev_dbg(iommu_dev->dev, "Allocated device id=%u\n", *id);
	return 0;
}

static struct qda_iommu_device *find_device_for_pid(struct qda_memory_manager *mem_mgr,
						    pid_t pid)
{
	unsigned long index;
	void *entry;
	struct qda_iommu_device *found_dev = NULL;
	unsigned long flags;

	xa_lock_irqsave(&mem_mgr->device_xa, flags);
	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		spin_lock(&iommu_dev->lock);
		if (iommu_dev->assigned_pid == pid) {
			found_dev = iommu_dev;
			refcount_inc(&found_dev->refcount);
			dev_dbg(found_dev->dev, "Reusing device id=%u for PID=%d (refcount=%u)\n",
				found_dev->id, pid, refcount_read(&found_dev->refcount));
			spin_unlock(&iommu_dev->lock);
			break;
		}
		spin_unlock(&iommu_dev->lock);
	}
	xa_unlock_irqrestore(&mem_mgr->device_xa, flags);

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

	xa_lock_irqsave(&mem_mgr->device_xa, flags);
	xa_for_each(&mem_mgr->device_xa, index, entry) {
		struct qda_iommu_device *iommu_dev = entry;

		spin_lock(&iommu_dev->lock);
		if (iommu_dev->assigned_pid == 0) {
			iommu_dev->assigned_pid = pid;
			iommu_dev->assigned_file_priv = file_priv;
			selected_dev = iommu_dev;
			refcount_set(&selected_dev->refcount, 1);
			dev_dbg(selected_dev->dev, "Assigned device id=%u to PID=%d\n",
				selected_dev->id, pid);
			spin_unlock(&iommu_dev->lock);
			break;
		}
		spin_unlock(&iommu_dev->lock);
	}
	xa_unlock_irqrestore(&mem_mgr->device_xa, flags);

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

/**
 * qda_memory_manager_assign_device() - Assign an IOMMU device to a process
 * @mem_mgr: Pointer to memory manager
 * @file_priv: DRM file private data for process association
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_assign_device(struct qda_memory_manager *mem_mgr,
				     struct drm_file *file_priv)
{
	struct qda_file_priv *qda_priv;
	struct qda_iommu_device *selected_dev = NULL;
	int ret = 0;
	pid_t current_pid;

	if (!file_priv || !file_priv->driver_priv) {
		pr_err("qda: Invalid file_priv or driver_priv\n");
		return -EINVAL;
	}

	qda_priv = (struct qda_file_priv *)file_priv->driver_priv;
	current_pid = qda_priv->pid;

	mutex_lock(&mem_mgr->process_assignment_lock);

	if (qda_priv->assigned_iommu_dev) {
		dev_dbg(qda_priv->assigned_iommu_dev->dev,
			"PID=%d already has device id=%u assigned\n",
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
		pr_err("qda: No available device for PID=%d\n", current_pid);
		ret = -ENOMEM;
		goto unlock_and_return;
	}

	qda_priv->assigned_iommu_dev = selected_dev;

unlock_and_return:
	mutex_unlock(&mem_mgr->process_assignment_lock);
	return ret;
}

static struct qda_iommu_device *get_or_assign_iommu_device(struct qda_memory_manager *mem_mgr,
							   struct drm_file *file_priv)
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

/**
 * qda_memory_manager_alloc() - Allocate memory for a GEM object
 * @mem_mgr: Pointer to memory manager
 * @gem_obj: Pointer to GEM object to allocate memory for
 * @file_priv: DRM file private data for process association
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_memory_manager_alloc(struct qda_memory_manager *mem_mgr, struct qda_gem_obj *gem_obj,
			     struct drm_file *file_priv)
{
	struct qda_iommu_device *selected_dev;
	size_t size;
	int ret;

	if (!mem_mgr || !gem_obj || !file_priv) {
		pr_err("qda: Invalid parameters for memory allocation\n");
		return -EINVAL;
	}

	size = gem_obj->size;
	if (size == 0) {
		drm_err(gem_obj->base.dev, "Invalid allocation size: 0\n");
		return -EINVAL;
	}

	selected_dev = get_or_assign_iommu_device(mem_mgr, file_priv);

	if (!selected_dev) {
		drm_err(gem_obj->base.dev,
			"Failed to get/assign device for allocation (size=%zu)\n",
			size);
		return -ENOMEM;
	}

	ret = qda_dma_alloc(selected_dev, gem_obj, size);
	if (ret) {
		drm_err(gem_obj->base.dev, "Allocation failed: size=%zu, device_id=%u, ret=%d\n",
			size, selected_dev->id, ret);
		return ret;
	}

	drm_dbg_driver(gem_obj->base.dev,
		       "Successfully allocated: size=%zu, device_id=%u, dma_addr=0x%llx\n",
		       size, selected_dev->id, gem_obj->dma_addr);
	return 0;
}

/**
 * qda_memory_manager_free() - Free memory for a GEM object
 * @mem_mgr: Pointer to memory manager
 * @gem_obj: Pointer to GEM object to free memory for
 */
void qda_memory_manager_free(struct qda_memory_manager *mem_mgr, struct qda_gem_obj *gem_obj)
{
	if (!gem_obj || !gem_obj->iommu_dev) {
		pr_debug("qda: Invalid gem_obj or iommu_dev for free\n");
		return;
	}

	qda_dma_free(gem_obj);
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

	init_iommu_device_fields(iommu_dev);

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
	mutex_init(&mem_mgr->process_assignment_lock);

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
