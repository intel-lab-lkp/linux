/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_DRM_RAS_TYPES_H_
#define _XE_DRM_RAS_TYPES_H_

#include <drm/xe_drm.h>
#include <linux/atomic.h>

struct drm_ras_node;

/**
 * struct xe_drm_ras_counter - xe ras counter
 *
 * This structure contains error class and counter information
 */
struct xe_drm_ras_counter {
	/** @name: error class name */
	const char *name;
	/** @counter: count of error */
	atomic64_t counter;
};

/**
 * struct xe_drm_ras - xe drm ras structure
 *
 * This structure has details of error counters
 */
struct xe_drm_ras {
	/** @node: DRM RAS node */
	struct drm_ras_node *node;

	/** @info: info array for all types of errors */
	struct xe_drm_ras_counter *info[DRM_XE_RAS_ERROR_SEVERITY_MAX];

};

#endif
