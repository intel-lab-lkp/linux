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
	/** @PANTHOR_HW_FEATURE_PWR_CONTROL: HW supports the PWR_CONTROL interface. */
	PANTHOR_HW_FEATURE_PWR_CONTROL,

	/** @PANTHOR_HW_FEATURES_END: Must be last. */
	PANTHOR_HW_FEATURES_END
};


/**
 * struct panthor_hw_ops - HW operations that are specific to a GPU
 */
struct panthor_hw_ops {
	/** @soft_reset: Soft reset function pointer */
	int (*soft_reset)(struct panthor_device *ptdev);
#define panthor_hw_soft_reset(__ptdev) \
	((__ptdev)->hw->ops.soft_reset ? (__ptdev)->hw->ops.soft_reset(__ptdev) : 0)

	/** @l2_power_off: L2 power off function pointer */
	int (*l2_power_off)(struct panthor_device *ptdev);
#define panthor_hw_l2_power_off(__ptdev) \
	((__ptdev)->hw->ops.l2_power_off ? (__ptdev)->hw->ops.l2_power_off(__ptdev) : 0)

	/** @l2_power_on: L2 power on function pointer */
	int (*l2_power_on)(struct panthor_device *ptdev);
#define panthor_hw_l2_power_on(__ptdev) \
	((__ptdev)->hw->ops.l2_power_on ? (__ptdev)->hw->ops.l2_power_on(__ptdev) : 0)
};

/**
 * struct panthor_hw - GPU specific register mapping and functions
 */
struct panthor_hw {
	/** @features: Bitmap containing panthor_hw_feature */
	DECLARE_BITMAP(features, PANTHOR_HW_FEATURES_END);

	/** @ops: Panthor HW specific operations */
	struct panthor_hw_ops ops;
};

int panthor_hw_init(struct panthor_device *ptdev);

bool panthor_hw_has_feature(struct panthor_device *ptdev, enum panthor_hw_feature feature);

#endif /* __PANTHOR_HW_H__ */
