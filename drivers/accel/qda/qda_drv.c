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
#include "qda_drv.h"
#include "qda_rpmsg.h"

DEFINE_DRM_ACCEL_FOPS(qda_accel_fops);

static struct drm_driver qda_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL,
	.fops			= &qda_accel_fops,
	.name = DRIVER_NAME,
	.desc = "Qualcomm DSP Accelerator Driver",
};

static void cleanup_drm_private(struct qda_dev *qdev)
{
	if (qdev->drm_priv) {
		qda_dbg(qdev, "Cleaning up DRM private data\n");
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
