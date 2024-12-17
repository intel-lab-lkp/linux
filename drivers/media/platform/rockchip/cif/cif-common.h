/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#ifndef _CIF_COMMON_H
#define _CIF_COMMON_H

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#include "cif-regs.h"

#define CIF_DRIVER_NAME "rockchip-cif"
#define CIF_DEFAULT_WIDTH 640
#define CIF_DEFAULT_HEIGHT 480
#define CIF_CLKS_MAX 4

enum cif_fmt_type {
	CIF_FMT_TYPE_YUV = 0,
	CIF_FMT_TYPE_RAW,
};

enum cif_plane_index {
	CIF_PLANE_Y = 0,
	CIF_PLANE_UV = 1,
};

struct cif_input_fmt {
	u32 mbus_code;

	enum cif_fmt_type fmt_type;
	enum v4l2_field field;

	union {
		u32 dvp_fmt_val;
	};
};

struct cif_output_fmt {
	u32 fourcc;
	u32 mbus_code;
	u8 cplanes;

	union {
		u32 dvp_fmt_val;
	};
};

struct cif_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	dma_addr_t buff_addr[VIDEO_MAX_PLANES];
	bool is_dummy;
};

struct cif_dummy_buffer {
	struct cif_buffer buffer;
	void *vaddr;
	u32 size;
};

struct cif_stream;

struct cif_remote {
	struct v4l2_async_connection async_conn;
	struct v4l2_subdev *sd;
	struct cif_stream *stream;
	int source_pad;
};

struct cif_stream {
	struct cif_device *cif_dev;
	struct cif_remote *remote;

	/* in ping-pong mode, two buffers can be provided to the HW */
	struct cif_buffer *buffers[2];
	int frame_idx;
	int frame_phase;

	/* in case of no available buffer, HW can write to the dummy buffer */
	struct cif_dummy_buffer dummy;

	bool stopping;
	wait_queue_head_t wq_stopped;

	/* queue of available buffers plus spinlock that protects it */
	spinlock_t driver_queue_lock;
	struct list_head driver_queue;

	/* Lock used by the V4L core. */
	struct mutex vlock;
	struct video_device vdev;
	struct vb2_queue buf_queue;
	struct media_pad pad;
	struct media_pipeline pipeline;

	struct v4l2_pix_format_mplane pix;
	const struct cif_input_fmt *active_in_fmt;
	const struct cif_output_fmt *active_out_fmt;

	const struct cif_input_fmt *in_fmts;
	unsigned int in_fmts_num;
	const struct cif_output_fmt *out_fmts;
	unsigned int out_fmts_num;

	void (*queue_buffer)(struct cif_stream *stream, unsigned int index);
	int (*start_streaming)(struct cif_stream *stream);
	void (*stop_streaming)(struct cif_stream *stream);
};

struct cif_dvp {
	struct cif_stream stream;
	struct v4l2_fwnode_endpoint vep;
	u32 cif_clk_delaynum;
};

struct cif_dvp_match_data {
	const struct cif_input_fmt *in_fmts;
	unsigned int in_fmts_num;
	const struct cif_output_fmt *out_fmts;
	unsigned int out_fmts_num;
	void (*setup)(struct cif_device *cif_dev);
	bool has_scaler;
	unsigned int regs[CIF_DVP_REGISTER_MAX];
};

struct cif_match_data {
	const char *const *clks;
	unsigned int clks_num;
	const struct cif_dvp_match_data *dvp;
};

struct cif_device {
	struct device *dev;

	struct clk_bulk_data clks[CIF_CLKS_MAX];
	unsigned int clks_num;
	struct regmap *grf;
	struct reset_control *cif_rst;
	int irq;
	void __iomem *base_addr;

	const struct cif_match_data *match_data;
	struct cif_dvp dvp;

	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
};

#endif
