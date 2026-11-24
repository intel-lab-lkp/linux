/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_COMMON_H
#define MALI_C720_COMMON_H

#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include "mali-c720-ctx-mgr.h"
#include "mali-c720-discovery.h"
#include "mali-c720-formats.h"
#include "mali-c720-id.h"
#include "mali-c720-interrupts.h"
#include "mali-c720-register-access.h"
#include "mali-c720-registers.h"
#include "mali-c720-registers-top.h"
#include "mali-c720-registers-ctx-mgr.h"
#include "mali-c720-registers-modules.h"

#define MALI_C720_FRAME_TIMESTAMP_COUNT	256

enum mali_c720_cap_devs {
	MALI_C720_CAP_DEV_FR0_RGB,
	MALI_C720_CAP_DEV_FR0_YUV,
	MALI_C720_MAX_NUM_CAP_DEVS,
};

struct mali_c720_context_regs {
	dma_addr_t dma_handle;
	u8 *base;
	size_t size;

	/* Serializes allocation, initialization and final release. */
	struct mutex allocation_lock;
	refcount_t users;
};

struct mali_c720_frame_timestamp {
	u64 timestamp;
	u32 sequence;
	bool valid;
};

struct mali_c720_context {
	unsigned int id;
	struct mali_c720 *mali_c720;
	struct mali_c720_context_regs regs;
	struct mali_c720_cdma_desc cdma_desc;
	struct mali_c720_slot_desc slot_desc;
	struct mali_c720_input_cfg input_cfg;
	unsigned int active_fifos_by_output[MALI_C720_MAX_NUM_CAP_DEVS];
	bool slot_desc_updated;

	/* Protects request submission */
	spinlock_t req_lock;
	unsigned int request_sequence;

	/* Protects context state and input_cfg. */
	spinlock_t state_lock;
	int state;
	bool pipeline_busy;

	unsigned int input_frame_sequence;
	unsigned int frame_sequence;
	struct mali_c720_frame_timestamp frame_timestamps[MALI_C720_FRAME_TIMESTAMP_COUNT];
	u64 timestamp;
};

struct mali_c720 {
	struct device *dev;
	unsigned int irq;
	u8 __iomem *base;
	resource_size_t mmio_size;
	u32 top_base;

	struct mali_c720_registers registers;
	u8 *default_regs;

	struct mali_c720_discovery discovery;
	const struct mali_c720_hw_version *hw_version;
	struct mali_c720_context context;
	struct mali_c720_ctx_mgr ctx_mgr;

	/* Serializes media registration, start/stop and resource allocation. */
	struct mutex device_lock;
	u8 num_active_contexts;

	DECLARE_KFIFO(irq_fifo, struct mali_c720_interrupt_data, MALI_C720_IRQ_FIFO_SIZE);

	/* Spinlock to guard IRQ fifo */
	spinlock_t irq_lock;
};

#endif /* MALI_C720_COMMON_H */
