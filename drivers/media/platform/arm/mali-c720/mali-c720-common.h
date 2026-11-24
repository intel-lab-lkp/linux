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

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

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

#define MALI_C720_MIN_INPUT_WIDTH	256
#define MALI_C720_MIN_INPUT_HEIGHT	256
#define MALI_C720_MAX_WIDTH		8192
#define MALI_C720_MAX_HEIGHT		4608
#define MALI_C720_DEFAULT_WIDTH		1920
#define MALI_C720_DEFAULT_HEIGHT	1080

#define MALI_C720_FRAME_TIMESTAMP_COUNT	256

struct mali_c720_subdev {
	struct v4l2_subdev sd;
	struct mali_c720 *mali_c720;
};

#define v4l2_sd_to_mali_c720_sd(_sd) \
	container_of(_sd, struct mali_c720_subdev, sd)

#define mali_c720_sd_to_sd(_mdev, type) \
	container_of(_mdev, type, mdev)

enum mali_c720_input_pads {
	MALI_C720_INPUT_PAD_SOURCE_SLOT_INPUT,
	MALI_C720_INPUT_PAD_SINK_INPUT0,
	MALI_C720_INPUT_NUM_PADS,
};

struct mali_c720_slot_input {
	struct mali_c720_buffer_pool raw_buffer_desc;
	struct media_pad *sink_pad;
	struct media_pad *src_pad;
	u32 stream;
	bool prepared;
};

struct mali_c720_input {
	struct mali_c720_subdev mdev;
	struct media_pad pads[MALI_C720_INPUT_NUM_PADS];
	struct mali_c720_slot_input slot_input;
};

enum mali_c720_isp_pads {
	MALI_C720_ISP_PAD_SINK_VIDEO,
	MALI_C720_ISP_PAD_SOURCE_HV,
	MALI_C720_ISP_NUM_PADS,
};

struct mali_c720_isp {
	struct mali_c720_subdev mdev;
	struct media_pad pads[MALI_C720_ISP_NUM_PADS];
	struct media_pad *remote_src;
	bool enabled;
};

enum mali_c720_vision_pipe_pads {
	MALI_C720_VISION_PIPE_PAD_SINK_VIDEO,
	MALI_C720_VISION_PIPE_PAD_SOURCE_RGB,
	MALI_C720_VISION_PIPE_NUM_PADS,
};

struct mali_c720_vision_pipe {
	struct mali_c720_subdev mdev;
	struct media_pad pads[MALI_C720_VISION_PIPE_NUM_PADS];
};

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

	struct v4l2_device v4l2_dev;

	struct mali_c720_input input;
	struct mali_c720_isp isp;
	struct mali_c720_vision_pipe vision_pipe;

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

/* ISP subdevice registration functions */

int mali_c720_register_input(struct mali_c720 *mali_c720);
void mali_c720_unregister_input(struct mali_c720 *mali_c720);
void mali_c720_cleanup_input(struct mali_c720 *mali_c720);

int mali_c720_register_isp(struct mali_c720 *mali_c720);
void mali_c720_unregister_isp(struct mali_c720 *mali_c720);
void mali_c720_cleanup_isp(struct mali_c720 *mali_c720);

int mali_c720_register_vision_pipe(struct mali_c720 *mali_c720);
void mali_c720_unregister_vision_pipe(struct mali_c720 *mali_c720);
void mali_c720_cleanup_vision_pipe(struct mali_c720 *mali_c720);

void mali_c720_isp_queue_event_sof(struct mali_c720 *mali_c720, u32 sequence);

#endif /* MALI_C720_COMMON_H */
