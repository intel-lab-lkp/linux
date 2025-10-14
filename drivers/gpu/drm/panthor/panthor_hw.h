/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_HW_H__
#define __PANTHOR_HW_H__

#include <linux/types.h>

struct panthor_device;

/**
 * enum panthor_hw_feature - Bit position of each HW feature
 *
 * Used to define GPU specific features based on the GPU architecture ID.
 * New feature flags will be added with support for newer GPU architectures.
 */
enum panthor_hw_feature {
	/** @PANTHOR_HW_FEATURES_END: Must be last. */
	PANTHOR_HW_FEATURES_END
};


/**
 * struct panthor_hw - GPU specific register mapping and functions
 */
struct panthor_hw {
	/** @features: Bitmap containing panthor_hw_feature */
	DECLARE_BITMAP(features, PANTHOR_HW_FEATURES_END);
};

int panthor_hw_init(struct panthor_device *ptdev);

bool panthor_hw_has_feature(struct panthor_device *ptdev, enum panthor_hw_feature feature);

#endif /* __PANTHOR_HW_H__ */
