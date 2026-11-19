/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_DEVICE_H__
#define __MALI_VPU_DEVICE_H__

#include <linux/device.h>

struct clk_bulk_data;
struct mali_vpu_variant;
struct reset_control_bulk_data;

/**
 * struct mali_vpu_dev - Mali VPU device
 * @dev: Reference to device structure.
 * @regs_base: MMIO base address for VPU registers.
 * @clk_list: List of VPU clocks.
 * @clk_count: Number of clocks in @clk_list.
 * @rst_list: List of reset controls.
 * @rst_count: Number of reset controls in @rst_list.
 * @irq: IRQ number for VPU.
 * @variant: Structure describing variant-specific features and capabilities.
 */
struct mali_vpu_dev {
	struct device			*dev;
	void __iomem			*regs_base;
	struct clk_bulk_data		*clk_list;
	unsigned int			clk_count;
	struct reset_control_bulk_data	*rst_list;
	unsigned int			rst_count;
	int				irq;

	const struct mali_vpu_variant	*variant;
};

#endif /* __MALI_VPU_DEVICE_H__ */
