/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_DRM_RAS_TYPES_H_
#define _XE_DRM_RAS_TYPES_H_

#include <linux/limits.h>

struct drm_ras_node;

/* Error categories reported by hardware */
enum hardware_error {
	HARDWARE_ERROR_CORRECTABLE = 0,
	HARDWARE_ERROR_NONFATAL = 1,
	HARDWARE_ERROR_FATAL = 2,
	HARDWARE_ERROR_MAX,
};

static inline const char *hw_error_to_str(const enum hardware_error hw_err)
{
	switch (hw_err) {
	case HARDWARE_ERROR_CORRECTABLE:
		return "correctable";
	case HARDWARE_ERROR_NONFATAL:
		return "nonfatal";
	case HARDWARE_ERROR_FATAL:
		return "fatal";
	default:
		return "UNKNOWN";
	}
}

struct xe_drm_ras_counter {
	const char *name;
	int counter;
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
	struct xe_drm_ras_counter *info[HARDWARE_ERROR_MAX];

};

#endif
