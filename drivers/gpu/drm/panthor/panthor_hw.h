/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_HW_H__
#define __PANTHOR_HW_H__

#include "panthor_device.h"

/**
 * struct panthor_hw_ops - HW operations that are specific to a GPU
 */
struct panthor_hw_ops {
	/** @soft_reset: Soft reset function pointer */
	int (*soft_reset)(struct panthor_device *ptdev);

	/** @l2_power_off: L2 power off function pointer */
	void (*l2_power_off)(struct panthor_device *ptdev);

	/** @l2_power_on: L2 power on function pointer */
	int (*l2_power_on)(struct panthor_device *ptdev);

	/** @power_changed_on: Start listening to power change IRQs */
	int (*power_changed_on)(struct panthor_device *ptdev);

	/** @power_changed_off: Stop listening to power change IRQs */
	void (*power_changed_off)(struct panthor_device *ptdev);
};

/**
 * struct panthor_hw_regmap - Register base addresses
 */
struct panthor_hw_regmap {
	/** @gpu_control_base: GPU_CONTROL base address */
	u32 gpu_control_base;

	/** @pwr_control_base: PWR_CONTROL base address */
	u32 pwr_control_base;

	/** @mcu_control_base: MCU_CONTROL base address */
	u32 mcu_control_base;

	struct {
		/** @mmu_as.base: MMU_AS base address */
		u32 base;

		/** @mmu_as.stride: Stride between subsequent MMU_AS register blocks */
		u32 stride;
	} mmu_as;
};

/**
 * struct panthor_hw - GPU specific register mapping and functions
 */
struct panthor_hw {
	/** @ops: Panthor HW specific operations */
	struct panthor_hw_ops ops;

	/** @map: Panthor HW-specific register base addresses */
	struct panthor_hw_regmap map;
};

int panthor_hw_init(struct panthor_device *ptdev);
int panthor_hw_power_status_register(void);
void panthor_hw_power_status_unregister(void);

static inline int panthor_hw_soft_reset(struct panthor_device *ptdev)
{
	return ptdev->hw->ops.soft_reset(ptdev);
}

static inline int panthor_hw_l2_power_on(struct panthor_device *ptdev)
{
	return ptdev->hw->ops.l2_power_on(ptdev);
}

static inline void panthor_hw_l2_power_off(struct panthor_device *ptdev)
{
	ptdev->hw->ops.l2_power_off(ptdev);
}

static inline bool panthor_hw_has_pwr_ctrl(struct panthor_device *ptdev)
{
	return ptdev->gpu_id.arch.major >= 14;
}

#endif /* __PANTHOR_HW_H__ */
