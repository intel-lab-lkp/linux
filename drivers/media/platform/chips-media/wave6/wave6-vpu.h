/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Wave6 series multi-standard codec IP - wave6 driver
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#ifndef __WAVE6_VPU_H__
#define __WAVE6_VPU_H__

#include <linux/device.h>
#include "wave6-vpu-thermal.h"
#include "wave6-vdi.h"
#include "wave6-vpuapi.h"

#define WAVE6_VPU_PLATFORM_DRIVER_NAME "wave6-vpu"

struct wave6_vpu_device;
struct vpu_core_device;

/**
 * enum wave6_vpu_state - VPU states
 * @WAVE6_VPU_STATE_OFF:	VPU is powered off
 * @WAVE6_VPU_STATE_PREPARE:	VPU is booting
 * @WAVE6_VPU_STATE_ON:		VPU is running
 * @WAVE6_VPU_STATE_SLEEP:	VPU is in a sleep mode
 */
enum wave6_vpu_state {
	WAVE6_VPU_STATE_OFF,
	WAVE6_VPU_STATE_PREPARE,
	WAVE6_VPU_STATE_ON,
	WAVE6_VPU_STATE_SLEEP
};

/**
 * struct wave6_vpu_dma_buf - VPU buffer from reserved memory or gen_pool
 * @size:	Buffer size
 * @dma_addr:	Mapped address for device access
 * @vaddr:	Kernel virtual address
 * @phys_addr:	Physical address of the reserved memory region or gen_pool
 *
 * Represents a buffer allocated from pre-reserved device memory regions or
 * SRAM via gen_pool_dma_alloc(). Used for code and SRAM buffers only.
 * Managed by the VPU device.
 */
struct wave6_vpu_dma_buf {
	size_t size;
	dma_addr_t dma_addr;
	void *vaddr;
	phys_addr_t phys_addr;
};

/**
 * struct wave6_vpu_resource - VPU device compatible data
 * @fw_name:	Firmware name for the device
 * @sram_size:	Required SRAM size
 */
struct wave6_vpu_resource {
	const char *fw_name;
	u32 sram_size;
};

/**
 * struct wave6_vpu_device - VPU driver structure
 * @get_vpu:		Function pointer, boot or wake the device
 * @put_vpu:		Function pointer, power off or suspend the device
 * @req_work_buffer:	Function pointer, request allocation of a work buffer
 * @dev:		Platform device pointer
 * @reg_base:		Base address of MMIO registers
 * @clks:		Array of clock handles
 * @num_clks:		Number of entries in @clks
 * @state:		Device state
 * @lock:		Mutex protecting device data, register access
 * @fw_available:	Firmware availability flag
 * @res:		Device compatible data
 * @sram_pool:		Genalloc pool for SRAM allocations
 * @sram_buf:		Optional SRAM buffer
 * @code_buf:		Firmware code buffer
 * @work_buffers:	Array of work buffers
 * @work_buffers_alloc:	Number of allocated work buffers
 * @work_buffers_avail:	Number of available work buffers
 * @thermal:		Thermal cooling device
 * @core_count:		Number of available VPU core devices
 *
 * @get_vpu, @put_vpu, @req_work_buffer are called by VPU core devices.
 *
 * Buffers such as @sram_buf, @code_buf, and @work_buffers are managed
 * by the VPU device and accessed exclusively by the firmware.
 */
struct wave6_vpu_device {
	int (*get_vpu)(struct wave6_vpu_device *vpu,
		       struct vpu_core_device *core);
	void (*put_vpu)(struct wave6_vpu_device *vpu,
			struct vpu_core_device *core);
	void (*req_work_buffer)(struct wave6_vpu_device *vpu,
				struct vpu_core_device *core);
	struct device *dev;
	void __iomem *reg_base;
	struct clk_bulk_data *clks;
	int num_clks;
	enum wave6_vpu_state state;
	struct mutex lock; /* Protects device data, register access */

	/* Prevents boot or sleep sequence if firmware is unavailable. */
	bool fw_available;

	const struct wave6_vpu_resource *res;
	struct gen_pool *sram_pool;
	struct wave6_vpu_dma_buf sram_buf;
	struct wave6_vpu_dma_buf code_buf;

	/* Allocates per-instance, used for storing instance-specific data. */
	struct vpu_buf work_buffers[MAX_NUM_INSTANCE];
	u32 work_buffers_alloc;
	u32 work_buffers_avail;

	struct vpu_thermal_cooling thermal;
	atomic_t core_count;
};

#endif /* __WAVE6_VPU_H__ */
