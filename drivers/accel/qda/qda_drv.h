/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_DRV_H__
#define __QDA_DRV_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>
#include <drm/drm_accel.h>
#include "qda_memory_manager.h"

/* Driver identification */
#define DRIVER_NAME "qda"

/**
 * struct qda_file_priv - Per-process private data for DRM file
 *
 * This structure tracks per-process state for each open file descriptor.
 * It maintains the IOMMU device assignment and links to the legacy qda_user
 * structure for compatibility with existing code.
 */
struct qda_file_priv {
	/* Process ID for tracking */
	pid_t pid;
	/* Pointer to qda_user structure for backward compatibility */
	struct qda_user *qda_user;
	/* IOMMU device assigned to this process */
	struct qda_iommu_device *assigned_iommu_dev;
};

/**
 * struct qda_user - Per-user context for remote processor interaction
 *
 * This structure maintains per-user state for interactions with the
 * remote processor, including memory mappings and pending operations.
 */
struct qda_user {
	/* Unique client identifier */
	u32 client_id;
	/* Back-pointer to device structure */
	struct qda_dev *qda_dev;
	/* GEM object for PD initialization memory */
	struct qda_gem_obj *init_mem_gem_obj;
};

/**
 * struct qda_drm_priv - DRM device private data for QDA device
 *
 * This structure serves as the DRM device private data (stored in dev_private),
 * bridging the DRM device context with the QDA device and providing access to
 * shared resources like the memory manager during buffer operations.
 */
struct qda_drm_priv {
	/* DRM device structure */
	struct drm_device *drm_dev;
	/* Global memory/IOMMU manager */
	struct qda_memory_manager *iommu_mgr;
	/* Back-pointer to qda_dev */
	struct qda_dev *qdev;
	/* Lock protecting import context */
	struct mutex import_lock;
	/* Current file_priv during prime import */
	struct drm_file *current_import_file_priv;
};

/* struct qda_dev - Main device structure for QDA driver */
struct qda_dev {
	/* DRM device for accelerator interface */
	struct drm_device *drm_dev;
	/* RPMsg device for communication with remote processor */
	struct rpmsg_device *rpdev;
	/* Underlying device structure */
	struct device *dev;
	/* Mutex protecting device state */
	struct mutex lock;
	/* IOMMU/memory manager */
	struct qda_memory_manager *iommu_mgr;
	/* DRM device private data */
	struct qda_drm_priv *drm_priv;
	/* Flag indicating device removal in progress */
	atomic_t removing;
	/* Atomic counter for generating unique client IDs */
	atomic_t client_id_counter;
	/* Name of the DSP (e.g., "cdsp", "adsp") */
	char dsp_name[16];
	/* Compute context-bank (CB) child devices */
	struct list_head cb_devs;
	/* XArray for context management */
	struct xarray ctx_xa;
};

/**
 * qda_get_log_device - Get appropriate device for logging
 * @qdev: QDA device structure
 *
 * Returns the most appropriate device structure for logging messages.
 * Prefers qdev->dev, falls back to qdev->drm_dev->dev, or returns NULL
 * if the device is being removed or invalid.
 */
static inline struct device *qda_get_log_device(struct qda_dev *qdev)
{
	if (!qdev || atomic_read(&qdev->removing))
		return NULL;

	if (qdev->dev)
		return qdev->dev;

	if (qdev->drm_dev)
		return qdev->drm_dev->dev;

	return NULL;
}

/*
 * Logging macros
 *
 * These macros provide consistent logging across the driver with automatic
 * function name inclusion. They use dev_* functions when a device is available,
 * falling back to pr_* functions otherwise.
 */

/* Error logging - always logs and tracks errors */
#define qda_err(qdev, fmt, ...) do { \
	struct device *__dev = qda_get_log_device(qdev); \
	if (__dev) \
		dev_err(__dev, "[%s] " fmt, __func__, ##__VA_ARGS__); \
	else \
		pr_err(DRIVER_NAME ": [%s] " fmt, __func__, ##__VA_ARGS__); \
} while (0)

/* Info logging - always logs, can be filtered via loglevel */
#define qda_info(qdev, fmt, ...) do { \
	struct device *__dev = qda_get_log_device(qdev); \
	if (__dev) \
		dev_info(__dev, "[%s] " fmt, __func__, ##__VA_ARGS__); \
	else \
		pr_info(DRIVER_NAME ": [%s] " fmt, __func__, ##__VA_ARGS__); \
} while (0)

/* Debug logging - controlled via dynamic debug (CONFIG_DYNAMIC_DEBUG) */
#define qda_dbg(qdev, fmt, ...) do { \
	struct device *__dev = qda_get_log_device(qdev); \
	if (__dev) \
		dev_dbg(__dev, "[%s] " fmt, __func__, ##__VA_ARGS__); \
	else \
		pr_debug(DRIVER_NAME ": [%s] " fmt, __func__, ##__VA_ARGS__); \
} while (0)

/*
 * Core device management functions
 */
int qda_init_device(struct qda_dev *qdev);
void qda_deinit_device(struct qda_dev *qdev);
int qda_register_device(struct qda_dev *qdev);
void qda_unregister_device(struct qda_dev *qdev);

/*
 * Utility function to get DRM private data from DRM device
 */
struct qda_drm_priv *get_drm_priv_from_device(struct drm_device *dev);

#endif /* __QDA_DRV_H__ */
