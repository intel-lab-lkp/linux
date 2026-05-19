// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/slab.h>
#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/qda_accel.h>

#include "qda_drv.h"
#include "qda_prime.h"
#include "qda_ioctl.h"
#include "qda_rpmsg.h"

static int qda_open(struct drm_device *dev, struct drm_file *file)
{
	struct qda_file_priv *qda_file_priv;

	qda_file_priv = kzalloc_obj(*qda_file_priv);
	if (!qda_file_priv)
		return -ENOMEM;

	qda_file_priv->pid = current->pid;
	qda_file_priv->qda_dev = qda_dev_from_drm(dev);
	qda_file_priv->remote_session_id =
		atomic_inc_return(&qda_file_priv->qda_dev->remote_session_id_counter);
	file->driver_priv = qda_file_priv;

	return 0;
}

static void qda_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct qda_file_priv *qda_file_priv = file->driver_priv;
	int idx;

	/* Only send the DSP release message while the device is accessible */
	if (drm_dev_enter(dev, &idx)) {
		qda_release_dsp_process(qda_file_priv->qda_dev, file);
		drm_dev_exit(idx);
	}

	if (qda_file_priv->assigned_iommu_dev) {
		struct qda_iommu_device *iommu_dev = qda_file_priv->assigned_iommu_dev;
		unsigned long flags;

		if (refcount_dec_and_test(&iommu_dev->refcount)) {
			spin_lock_irqsave(&iommu_dev->lock, flags);
			iommu_dev->assigned_pid = 0;
			iommu_dev->assigned_file_priv = NULL;
			spin_unlock_irqrestore(&iommu_dev->lock, flags);
		}
	}

	kfree(qda_file_priv);
	file->driver_priv = NULL;
}

DEFINE_DRM_ACCEL_FOPS(qda_accel_fops);

static const struct drm_ioctl_desc qda_ioctls[] = {
	DRM_IOCTL_DEF_DRV(QDA_QUERY, qda_ioctl_query, 0),
	DRM_IOCTL_DEF_DRV(QDA_GEM_CREATE, qda_ioctl_gem_create, 0),
	DRM_IOCTL_DEF_DRV(QDA_GEM_MMAP_OFFSET, qda_ioctl_gem_mmap_offset, 0),
	DRM_IOCTL_DEF_DRV(QDA_REMOTE_SESSION_CREATE, qda_ioctl_init_create, 0),
	DRM_IOCTL_DEF_DRV(QDA_REMOTE_MAP, qda_ioctl_mmap, 0),
	DRM_IOCTL_DEF_DRV(QDA_REMOTE_INVOKE, qda_ioctl_invoke, 0),
};

static const struct drm_driver qda_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.fops = &qda_accel_fops,
	.open = qda_open,
	.postclose = qda_postclose,
	.ioctls = qda_ioctls,
	.num_ioctls = ARRAY_SIZE(qda_ioctls),
	.gem_prime_import = qda_gem_prime_import,
	.prime_fd_to_handle = qda_prime_fd_to_handle,
	.name = QDA_DRIVER_NAME,
	.desc = "Qualcomm DSP Accelerator Driver",
};

struct qda_dev *qda_alloc_device(struct device *dev)
{
	struct qda_dev *qdev;

	qdev = devm_drm_dev_alloc(dev, &qda_drm_driver, struct qda_dev, drm_dev);
	if (IS_ERR(qdev))
		return ERR_CAST(qdev);

	INIT_LIST_HEAD(&qdev->cb_devs);
	return qdev;
}

static void cleanup_memory_manager(struct qda_dev *qdev)
{
	if (qdev->iommu_mgr) {
		qda_memory_manager_exit(qdev->iommu_mgr);
		kfree(qdev->iommu_mgr);
		qdev->iommu_mgr = NULL;
	}
}

static void cleanup_device_resources(struct qda_dev *qdev)
{
	xa_destroy(&qdev->ctx_xa);
}

static void init_device_resources(struct qda_dev *qdev)
{
	atomic_set(&qdev->remote_session_id_counter, 0);
	xa_init_flags(&qdev->ctx_xa, XA_FLAGS_ALLOC1);
}

static int init_memory_manager(struct qda_dev *qdev)
{
	qdev->iommu_mgr = kzalloc_obj(*qdev->iommu_mgr);
	if (!qdev->iommu_mgr)
		return -ENOMEM;

	return qda_memory_manager_init(qdev->iommu_mgr);
}

void qda_deinit_device(struct qda_dev *qdev)
{
	mutex_destroy(&qdev->import_lock);
	cleanup_memory_manager(qdev);
	cleanup_device_resources(qdev);
}

int qda_init_device(struct qda_dev *qdev)
{
	int ret;

	mutex_init(&qdev->import_lock);
	qdev->current_import_file_priv = NULL;
	init_device_resources(qdev);

	ret = init_memory_manager(qdev);
	if (ret) {
		drm_err(&qdev->drm_dev, "Failed to initialize memory manager: %d\n", ret);
		cleanup_device_resources(qdev);
		mutex_destroy(&qdev->import_lock);
	}

	return ret;
}

void qda_unregister_device(struct qda_dev *qdev)
{
	drm_dev_unregister(&qdev->drm_dev);
}

int qda_register_device(struct qda_dev *qdev)
{
	int ret;

	ret = drm_dev_register(&qdev->drm_dev, 0);
	if (ret)
		drm_err(&qdev->drm_dev, "Failed to register DRM device: %d\n", ret);

	return ret;
}

static int __init qda_core_init(void)
{
	int ret;

	ret = qda_rpmsg_register();
	if (ret)
		return ret;

	pr_info("qda: QDA driver initialization complete\n");
	return 0;
}

static void __exit qda_core_exit(void)
{
	qda_rpmsg_unregister();
}

module_init(qda_core_init);
module_exit(qda_core_exit);

MODULE_AUTHOR("Qualcomm AI Infra Team");
MODULE_DESCRIPTION("Qualcomm DSP Accelerator Driver");
MODULE_LICENSE("GPL");
