/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_COMMON_H
#define MALI_C720_COMMON_H

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

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

#define MALI_C720_DRIVER_NAME		"mali-c720"

#define MALI_C720_MIN_INPUT_WIDTH	256
#define MALI_C720_MIN_INPUT_HEIGHT	256
#define MALI_C720_MIN_OUTPUT_WIDTH	64
#define MALI_C720_MIN_OUTPUT_HEIGHT	64
#define MALI_C720_MAX_WIDTH		8192
#define MALI_C720_MAX_HEIGHT		4608
#define MALI_C720_DEFAULT_WIDTH		1920
#define MALI_C720_DEFAULT_HEIGHT	1080

#define MALI_C720_MAX_DOWNSCALE_RATIO	0xffffff /* Q4.20 */

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

enum mali_c720_fr_pipe_pads {
	MALI_C720_FR_PIPE_PAD_SINK_VIDEO,
	MALI_C720_FR_PIPE_PAD_SOURCE_RGB,
	MALI_C720_FR_PIPE_PAD_SOURCE_YUV,
	MALI_C720_FR_PIPE_NUM_PADS,
};

struct mali_c720_crop_config {
	u32 start_x;
	u32 start_y;
	u32 size_x;
	u32 size_y;
	bool bypass;
};

struct mali_c720_fr_resizer_config {
	struct mali_c720_crop_config crop;
	u32 hfilt_tincr;
	u32 vfilt_tincr;
	unsigned int hbank;
	unsigned int vbank;
	bool hfilt_enable;
	bool vfilt_enable;
};

struct mali_c720_fr_pipe {
	struct mali_c720_subdev mdev;
	struct media_pad pads[MALI_C720_FR_PIPE_NUM_PADS];
	struct media_pad *remote_src;
	u32 crop_base;
	u32 scaler_base;
	u32 scaler_hmem_base;
	u32 scaler_vmem_base;
	u32 output_formatter_base;

	/* Protect resizer_config, resizer_updated and enabled_crops. */
	spinlock_t resizer_lock;
	struct mali_c720_fr_resizer_config resizer_config;
	bool resizer_updated;
	unsigned long enabled_crops;
};

enum mali_c720_cap_devs {
	MALI_C720_CAP_DEV_FR0_RGB,
	MALI_C720_CAP_DEV_FR0_YUV,
	MALI_C720_MAX_NUM_CAP_DEVS,
};

struct mali_c720_cap_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	dma_addr_t addr;
};

struct mali_c720_cap_dev {
	unsigned int id;
	struct mali_c720 *mali_c720;
	struct v4l2_subdev *src_sd;
	u32 src_pad;
	struct video_device vdev;
	struct media_pad pad;
	struct vb2_queue queue;
	/* Mutex to provide to vb2 */
	struct mutex lock;

	struct {
		const struct mali_c720_v4l2_format_info *info;
		struct v4l2_pix_format_mplane pix_mp;
		struct mali_c720_buffer_request_info buffer_request;
		u32 default_format;
	} format;

	struct {
		/* Spinlock to guard buffer queue */
		spinlock_t lock;
		struct list_head input;
		struct list_head processing;
		struct mali_c720_buffer_pool pool;
	} buffers;
};

struct mali_c720_request {
	struct media_request req;
	unsigned int queue_sequence;
	bool submitted;
};

static inline struct mali_c720_request *
to_mali_c720_request(struct media_request *req)
{
	return container_of(req, struct mali_c720_request, req);
}

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
	struct clk *clk;
	struct reset_control *reset;

	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct media_pipeline pipe;

	struct mali_c720_input input;
	struct mali_c720_isp isp;
	struct mali_c720_vision_pipe vision_pipe;
	struct mali_c720_fr_pipe fr_pipe;
	struct mali_c720_cap_dev cap_devs[MALI_C720_MAX_NUM_CAP_DEVS];

	unsigned int num_async_endpoints;

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

int mali_c720_register_fr_pipe(struct mali_c720 *mali_c720);
void mali_c720_unregister_fr_pipe(struct mali_c720 *mali_c720);
void mali_c720_cleanup_fr_pipe(struct mali_c720 *mali_c720);

int mali_c720_register_capture_devs(struct mali_c720 *mali_c720);
void mali_c720_unregister_capture_devs(struct mali_c720 *mali_c720);
void mali_c720_cleanup_capture_devs(struct mali_c720 *mali_c720);

void mali_c720_fr_pipe_apply_resizer_config(struct mali_c720_fr_pipe *pipe);

bool mali_c720_cap_dev_uses_requests(struct mali_c720_cap_dev *cap_dev);
struct media_request *mali_c720_cap_dev_get_request(struct mali_c720_cap_dev *cap_dev);
int mali_c720_cap_dev_is_ready(struct mali_c720_cap_dev *cap_dev);
void mali_c720_cap_dev_set_next_buffer(struct mali_c720_cap_dev *cap_dev,
				       struct media_request *req);
void mali_c720_cap_dev_buffer_done(struct mali_c720_cap_dev *cap_dev,
				   bool success);

void mali_c720_isp_queue_event_sof(struct mali_c720 *mali_c720, u32 sequence);

int mali_c720_pipeline_ready(struct mali_c720 *mali_c720);

int mali_c720_streamon(struct file *file, void *priv, enum v4l2_buf_type type);

void mali_c720_try_schedule_frame(struct mali_c720_context *ctx);

#endif /* MALI_C720_COMMON_H */
