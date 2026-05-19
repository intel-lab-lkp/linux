/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_DRV_H__
#define __QDA_DRV_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/rpmsg.h>
#include <linux/types.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include "qda_memory_manager.h"

/* Driver identification */
#define QDA_DRIVER_NAME "qda"

/**
 * struct qda_file_priv - Per-process private data for DRM file
 */
struct qda_file_priv {
	/** @qda_dev: Back-pointer to device structure */
	struct qda_dev *qda_dev;
	/** @assigned_iommu_dev: IOMMU device assigned to this process */
	struct qda_iommu_device *assigned_iommu_dev;
	/** @pid: Process ID for tracking */
	pid_t pid;
};

/**
 * struct qda_dev - Main device structure for QDA driver
 *
 * The DRM device is embedded as the first member so that container_of()
 * can recover the qda_dev from any drm_device pointer.
 */
struct qda_dev {
	/** @drm_dev: Embedded DRM device; recover via qda_dev_from_drm() */
	struct drm_device drm_dev;
	/** @rpdev: RPMsg device for communication with the remote processor */
	struct rpmsg_device *rpdev;
	/** @dev: Underlying Linux device */
	struct device *dev;
	/** @cb_devs: Compute context-bank (CB) child devices */
	struct list_head cb_devs;
	/** @iommu_mgr: IOMMU/memory manager instance */
	struct qda_memory_manager *iommu_mgr;
	/** @dsp_name: Name of the DSP domain (e.g. "cdsp", "adsp") */
	const char *dsp_name;
};

/**
 * qda_dev_from_drm - Recover qda_dev from an embedded drm_device pointer
 * @dev: Pointer to the embedded drm_device
 *
 * Return: Pointer to the enclosing qda_dev.
 */
static inline struct qda_dev *qda_dev_from_drm(struct drm_device *dev)
{
	return container_of(dev, struct qda_dev, drm_dev);
}

/* Device allocation (uses devm_drm_dev_alloc internally) */
struct qda_dev *qda_alloc_device(struct device *dev);

/* Core device lifecycle */
int qda_init_device(struct qda_dev *qdev);
void qda_deinit_device(struct qda_dev *qdev);
int qda_register_device(struct qda_dev *qdev);
void qda_unregister_device(struct qda_dev *qdev);

#endif /* __QDA_DRV_H__ */
