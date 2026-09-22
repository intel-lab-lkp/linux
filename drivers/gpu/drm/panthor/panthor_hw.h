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

/** struct panthor_hw_regbank - Register offset and instance stride information */
struct panthor_hw_regbank {
	/** @offset: Register offset relative to &struct panthor_device.iomem */
	u32 offset;

	/**
	 * @stride: Register offset between instances, or 0 for non-instanced
	 * registers
	 */
	u32 stride;
};

/** struct panthor_hw_regs_layout - Hardware register bank layout */
struct panthor_hw_regs_layout {
	/** @gpu_control: GPU_CONTROL register bank layout */
	struct panthor_hw_regbank gpu_control;

	/** @pwr_control: PWR_CONTROL register bank layout */
	struct panthor_hw_regbank pwr_control;

	/** @mcu_control: MCU_CONTROL register bank layout */
	struct panthor_hw_regbank mcu_control;

	/** @mmu_as_control: MMU_AS_CONTROL register bank layout */
	struct panthor_hw_regbank mmu_as_control;
};

/**
 * struct panthor_hw - GPU specific register mapping and functions
 */
struct panthor_hw {
	/** @ops: Panthor HW specific operations */
	struct panthor_hw_ops ops;

	/** @regs: Hardware register bank layout */
	struct panthor_hw_regs_layout regs;
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
	return ptdev->gpu_id.arch_major >= 14;
}

#endif /* __PANTHOR_HW_H__ */
