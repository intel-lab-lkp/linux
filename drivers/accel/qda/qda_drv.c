// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/qda_accel.h>
#include <drm/drm_prime.h>

#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_prime.h"
#include "qda_ioctl.h"
#include "qda_rpmsg.h"

struct qda_drm_priv *get_drm_priv_from_device(struct drm_device *dev)
{
	if (!dev)
		return NULL;

	return (struct qda_drm_priv *)dev->dev_private;
}

static struct qda_dev *get_qdev_from_drm_device(struct drm_device *dev)
{
	struct qda_drm_priv *drm_priv;

	if (!dev) {
		qda_dbg(NULL, "Invalid drm_device\n");
		return NULL;
	}

	drm_priv = get_drm_priv_from_device(dev);
	if (!drm_priv) {
		qda_dbg(NULL, "No drm_priv in dev_private\n");
		return NULL;
	}

	return drm_priv->qdev;
}

static struct qda_user *alloc_qda_user(struct qda_dev *qdev)
{
	struct qda_user *qda_user;

	qda_user = kzalloc_obj(*qda_user, GFP_KERNEL);
	if (!qda_user)
		return NULL;

	qda_user->client_id = atomic_inc_return(&qdev->client_id_counter);
	qda_user->qda_dev = qdev;

	qda_dbg(qdev, "Allocated qda_user with client_id=%u\n", qda_user->client_id);
	return qda_user;
}

static void free_qda_user(struct qda_user *qda_user)
{
	if (!qda_user)
		return;

	qda_dbg(qda_user->qda_dev, "Freeing qda_user client_id=%u\n", qda_user->client_id);

	kfree(qda_user);
}

static int qda_open(struct drm_device *dev, struct drm_file *file)
{
	struct qda_user *qda_user;
	struct qda_file_priv *qda_file_priv;
	struct qda_dev *qdev;

	if (!file) {
		qda_dbg(NULL, "Invalid file pointer\n");
		return -EINVAL;
	}

	qdev = get_qdev_from_drm_device(dev);
	if (!qdev) {
		qda_dbg(NULL, "Failed to get qdev from drm_device\n");
		return -EINVAL;
	}

	qda_file_priv = kzalloc(sizeof(*qda_file_priv), GFP_KERNEL);
	if (!qda_file_priv)
		return -ENOMEM;

	qda_file_priv->pid = current->pid;
	qda_file_priv->assigned_iommu_dev = NULL; /* Will be assigned on first allocation */

	qda_user = alloc_qda_user(qdev);
	if (!qda_user) {
		qda_dbg(qdev, "Failed to allocate qda_user\n");
		kfree(qda_file_priv);
		return -ENOMEM;
	}

	file->driver_priv = qda_file_priv;
	qda_file_priv->qda_user = qda_user;

	qda_dbg(qdev, "Device opened successfully for PID %d\n", current->pid);

	return 0;
}

static void qda_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct qda_dev *qdev;
	struct qda_file_priv *qda_file_priv;
	struct qda_user *qda_user;

	qdev = get_qdev_from_drm_device(dev);
	if (!qdev || atomic_read(&qdev->removing)) {
		qda_dbg(NULL, "Device unavailable or removing\n");
		return;
	}

	qda_file_priv = (struct qda_file_priv *)file->driver_priv;
	if (qda_file_priv) {
		if (qda_file_priv->assigned_iommu_dev) {
			struct qda_iommu_device *iommu_dev = qda_file_priv->assigned_iommu_dev;
			unsigned long flags;

			/* Decrement reference count - if it reaches 0, reset PID assignment */
			if (refcount_dec_and_test(&iommu_dev->refcount)) {
				/* Last reference released - reset PID assignment */
				spin_lock_irqsave(&iommu_dev->lock, flags);
				iommu_dev->assigned_pid = 0;
				iommu_dev->assigned_file_priv = NULL;
				spin_unlock_irqrestore(&iommu_dev->lock, flags);

				qda_dbg(qdev, "Reset PID assignment for IOMMU device %u (process %d exited)\n",
					iommu_dev->id, qda_file_priv->pid);
			} else {
				qda_dbg(qdev, "Decremented reference for IOMMU device %u from process %d\n",
					iommu_dev->id, qda_file_priv->pid);
			}
		}

		qda_user = qda_file_priv->qda_user;
		if (qda_user)
			free_qda_user(qda_user);

		kfree(qda_file_priv);
		file->driver_priv = NULL;
	}

	qda_dbg(qdev, "Device closed for PID %d\n", current->pid);
}

DEFINE_DRM_ACCEL_FOPS(qda_accel_fops);

static const struct drm_ioctl_desc qda_ioctls[] = {
	DRM_IOCTL_DEF_DRV(QDA_QUERY, qda_ioctl_query, 0),
	DRM_IOCTL_DEF_DRV(QDA_GEM_CREATE, qda_ioctl_gem_create, 0),
	DRM_IOCTL_DEF_DRV(QDA_GEM_MMAP_OFFSET, qda_ioctl_gem_mmap_offset, 0),
};

static struct drm_driver qda_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.fops			= &qda_accel_fops,
	.open			= qda_open,
	.postclose		= qda_postclose,
	.ioctls = qda_ioctls,
	.num_ioctls = ARRAY_SIZE(qda_ioctls),
	.gem_prime_import = qda_gem_prime_import,
	.prime_fd_to_handle = qda_ioctl_prime_fd_to_handle,
	.name = DRIVER_NAME,
	.desc = "Qualcomm DSP Accelerator Driver",
};

static void cleanup_drm_private(struct qda_dev *qdev)
{
	if (qdev->drm_priv) {
		qda_dbg(qdev, "Cleaning up DRM private data\n");
		mutex_destroy(&qdev->drm_priv->import_lock);
		kfree(qdev->drm_priv);
	}
}

static void cleanup_iommu_manager(struct qda_dev *qdev)
{
	if (qdev->iommu_mgr) {
		qda_dbg(qdev, "Cleaning up IOMMU manager\n");
		qda_memory_manager_exit(qdev->iommu_mgr);
		kfree(qdev->iommu_mgr);
		qdev->iommu_mgr = NULL;
	}
}

static void cleanup_device_resources(struct qda_dev *qdev)
{
	mutex_destroy(&qdev->lock);
}

void qda_deinit_device(struct qda_dev *qdev)
{
	cleanup_drm_private(qdev);
	cleanup_iommu_manager(qdev);
	cleanup_device_resources(qdev);
}

/* Initialize device resources */
static void init_device_resources(struct qda_dev *qdev)
{
	qda_dbg(qdev, "Initializing device resources\n");

	mutex_init(&qdev->lock);
	atomic_set(&qdev->removing, 0);
	atomic_set(&qdev->client_id_counter, 0);
}

static int init_memory_manager(struct qda_dev *qdev)
{
	int ret;

	qda_dbg(qdev, "Initializing IOMMU manager\n");

	qdev->iommu_mgr = kzalloc_obj(*qdev->iommu_mgr, GFP_KERNEL);
	if (!qdev->iommu_mgr)
		return -ENOMEM;

	ret = qda_memory_manager_init(qdev->iommu_mgr);
	if (ret) {
		qda_err(qdev, "Failed to initialize memory manager: %d\n", ret);
		kfree(qdev->iommu_mgr);
		qdev->iommu_mgr = NULL;
		return ret;
	}

	qda_dbg(qdev, "IOMMU manager initialized successfully\n");
	return 0;
}

static int init_drm_private(struct qda_dev *qdev)
{
	qda_dbg(qdev, "Initializing DRM private data\n");

	qdev->drm_priv = kzalloc_obj(*qdev->drm_priv, GFP_KERNEL);
	if (!qdev->drm_priv)
		return -ENOMEM;

	mutex_init(&qdev->drm_priv->import_lock);
	qdev->drm_priv->current_import_file_priv = NULL;

	qda_dbg(qdev, "DRM private data initialized successfully\n");
	return 0;
}

int qda_init_device(struct qda_dev *qdev)
{
	int ret;

	init_device_resources(qdev);

	ret = init_memory_manager(qdev);
	if (ret) {
		qda_err(qdev, "IOMMU manager initialization failed: %d\n", ret);
		goto err_cleanup_resources;
	}

	ret = init_drm_private(qdev);
	if (ret) {
		qda_err(qdev, "DRM private data initialization failed: %d\n", ret);
		goto err_cleanup_iommu;
	}

	qda_dbg(qdev, "QDA device initialized successfully\n");
	return 0;

err_cleanup_iommu:
	cleanup_iommu_manager(qdev);
err_cleanup_resources:
	cleanup_device_resources(qdev);
	return ret;
}

static int setup_and_register_drm_device(struct qda_dev *qdev)
{
	struct drm_device *ddev;
	int ret;

	qda_dbg(qdev, "Setting up and registering DRM device\n");

	ddev = drm_dev_alloc(&qda_drm_driver, qdev->dev);
	if (IS_ERR(ddev)) {
		ret = PTR_ERR(ddev);
		qda_err(qdev, "Failed to allocate DRM device: %d\n", ret);
		return ret;
	}

	qdev->drm_priv->drm_dev = ddev;
	qdev->drm_priv->iommu_mgr = qdev->iommu_mgr;
	qdev->drm_priv->qdev = qdev;

	ddev->dev_private = qdev->drm_priv;
	qdev->drm_dev = ddev;

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		qda_err(qdev, "Failed to register DRM device: %d\n", ret);
		drm_dev_put(ddev);
		return ret;
	}

	qda_dbg(qdev, "DRM device registered successfully\n");
	return 0;
}

int qda_register_device(struct qda_dev *qdev)
{
	int ret;

	ret = setup_and_register_drm_device(qdev);
	if (ret) {
		qda_err(qdev, "DRM device setup failed: %d\n", ret);
		return ret;
	}

	qda_dbg(qdev, "QDA device registered successfully\n");
	return 0;
}

void qda_unregister_device(struct qda_dev *qdev)
{
	qda_info(qdev, "Unregistering QDA device\n");

	if (qdev->drm_dev) {
		qda_dbg(qdev, "Unregistering DRM device\n");
		drm_dev_unregister(qdev->drm_dev);
		drm_dev_put(qdev->drm_dev);
		qdev->drm_dev = NULL;
	}

	qda_dbg(qdev, "QDA device unregistered successfully\n");
}

static int __init qda_core_init(void)
{
	int ret;

	ret = qda_rpmsg_register();
	if (ret)
		return ret;

	qda_info(NULL, "QDA driver initialization complete\n");
	return 0;
}

static void __exit qda_core_exit(void)
{
	qda_rpmsg_unregister();
	qda_info(NULL, "QDA driver exit complete\n");
}

module_init(qda_core_init);
module_exit(qda_core_exit);

MODULE_AUTHOR("Qualcomm AI Infra Team");
MODULE_DESCRIPTION("Qualcomm DSP Accelerator Driver");
MODULE_LICENSE("GPL");
