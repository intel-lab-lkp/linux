/* SPDX-License-Identifier: MIT */
/* Copyright © 2025 Intel Corporation x*/

#ifndef __DISPLAY_INTERFACE_H__
#define __DISPLAY_INTERFACE_H__

#include <linux/types.h>

struct drm_device;

/**
 * struct intel_core_hooks - services core provides to display
 */
struct intel_core_hooks {
	/** @has_flat_ccs: does the device support flat CCS */
	bool (*has_flat_ccs)(struct drm_device *drm);
};

#endif
