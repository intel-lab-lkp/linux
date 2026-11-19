/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_DEVICE_H__
#define __MALI_VPU_DEVICE_H__

#include <linux/device.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>

#include "mali-vpu-scheduler.h"

#define MALI_VPU_DRV_NAME "mali-vpu"

struct clk_bulk_data;
struct v4l2_m2m_dev;
struct mali_vpu_variant;
struct mali_vpu_fw_cache;
struct reset_control_bulk_data;

/**
 * enum mali_vpu_session_role - Mali VPU session role
 * @MALI_VPU_DECODER: Video decoding session
 */
enum mali_vpu_session_role {
	MALI_VPU_DECODER = 0,
};

/**
 * struct mali_vpu_dev - Mali VPU device
 * @dev: Reference to device structure.
 * @regs_base: MMIO base address for VPU registers.
 * @clk_list: List of VPU clocks.
 * @clk_count: Number of clocks in @clk_list.
 * @rst_list: List of reset controls.
 * @rst_count: Number of reset controls in @rst_list.
 * @dma_mask: DMA mask for the device, indicating supported address width.
 * @irq: IRQ number for VPU.
 * @irqve_mask: Bitmap of pending LSID IRQs.
 * @irq_wq: Workqueue used to process LSID IRQs outside hard IRQ context.
 * @irq_work: Work item for deferred LSID IRQ handling.
 * @variant: Structure describing variant-specific features and capabilities.
 * @v4l2_dev: V4L2 device structure for mali vpu.
 * @vdev_dec: Video device structure for decoder.
 * @m2m_dev: V4L2 mem2mem device structure for mali vpu.
 * @fw_cache: Firmware cache for sharing or reusing firmware blobs.
 * @sched: Mali VPU hardware scheduler state.
 */
struct mali_vpu_dev {
	struct device			*dev;
	void __iomem			*regs_base;
	struct clk_bulk_data		*clk_list;
	unsigned int			clk_count;
	struct reset_control_bulk_data	*rst_list;
	unsigned int			rst_count;
	u64				dma_mask;
	int				irq;
	unsigned long			irqve_mask;
	struct workqueue_struct		*irq_wq;
	struct work_struct		irq_work;

	const struct mali_vpu_variant	*variant;
	struct v4l2_device		v4l2_dev;
	struct video_device		*vdev_dec;
	struct v4l2_m2m_dev		*m2m_dev;
	struct mali_vpu_fw_cache	*fw_cache;
	struct mali_vpu_sched		sched;
};

#endif /* __MALI_VPU_DEVICE_H__ */
