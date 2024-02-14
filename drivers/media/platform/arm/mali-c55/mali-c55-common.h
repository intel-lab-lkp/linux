/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM Mali-C55 ISP Driver - Common definitions
 *
 * Copyright (C) 2023 Ideas on Board Oy
 */

#ifndef _MALI_C55_COMMON_H
#define _MALI_C55_COMMON_H

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/videodev2.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

#define MALI_C55_DRIVER_NAME		"mali-c55"

/* min and max values for the image sizes */
#define MALI_C55_MIN_WIDTH		640
#define MALI_C55_MIN_HEIGHT		480
#define MALI_C55_MAX_WIDTH		8192
#define MALI_C55_MAX_HEIGHT		8192
#define MALI_C55_DEFAULT_WIDTH		1920
#define MALI_C55_DEFAULT_HEIGHT		1080

#define MALI_C55_DEFAULT_MEDIA_BUS_FMT	MEDIA_BUS_FMT_RGB121212_1X36

struct mali_c55;
struct mali_c55_cap_dev;
struct platform_device;

static const char * const mali_c55_clk_names[] = {
	"vclk",
	"aclk",
	"hclk",
};

enum mali_c55_interrupts {
	MALI_C55_IRQ_ISP_START,
	MALI_C55_IRQ_ISP_DONE,
	MALI_C55_IRQ_MCM_ERROR,
	MALI_C55_IRQ_BROKEN_FRAME_ERROR,
	MALI_C55_IRQ_MET_AF_DONE,
	MALI_C55_IRQ_MET_AEXP_DONE,
	MALI_C55_IRQ_MET_AWB_DONE,
	MALI_C55_IRQ_AEXP_1024_DONE,
	MALI_C55_IRQ_IRIDIX_MET_DONE,
	MALI_C55_IRQ_LUT_INIT_DONE,
	MALI_C55_IRQ_FR_Y_DONE,
	MALI_C55_IRQ_FR_UV_DONE,
	MALI_C55_IRQ_DS_Y_DONE,
	MALI_C55_IRQ_DS_UV_DONE,
	MALI_C55_IRQ_LINEARIZATION_DONE,
	MALI_C55_IRQ_RAW_FRONTEND_DONE,
	MALI_C55_IRQ_NOISE_REDUCTION_DONE,
	MALI_C55_IRQ_IRIDIX_DONE,
	MALI_C55_IRQ_BAYER2RGB_DONE,
	MALI_C55_IRQ_WATCHDOG_TIMER,
	MALI_C55_IRQ_FRAME_COLLISION,
	MALI_C55_IRQ_UNUSED,
	MALI_C55_IRQ_DMA_ERROR,
	MALI_C55_IRQ_INPUT_STOPPED,
	MALI_C55_IRQ_MET_AWB_TARGET1_HIT,
	MALI_C55_IRQ_MET_AWB_TARGET2_HIT,
	MALI_C55_NUM_IRQ_BITS
};

enum mali_c55_isp_pads {
	MALI_C55_ISP_PAD_SINK_VIDEO,
	MALI_C55_ISP_PAD_SOURCE,
	MALI_C55_ISP_PAD_SOURCE_BYPASS,
	MALI_C55_ISP_NUM_PADS,
};

struct mali_c55_tpg {
	struct mali_c55 *mali_c55;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct mutex lock;
	struct mali_c55_tpg_ctrls {
		struct v4l2_ctrl_handler handler;
		struct v4l2_ctrl *test_pattern;
		struct v4l2_ctrl *hblank;
		struct v4l2_ctrl *vblank;
	} ctrls;
};

struct mali_c55_isp {
	struct mali_c55 *mali_c55;
	struct v4l2_subdev sd;
	struct media_pad pads[MALI_C55_ISP_NUM_PADS];
	struct v4l2_subdev *source;
	struct v4l2_async_notifier notifier;
	struct mutex lock;
};

enum mali_c55_resizer_ids {
	MALI_C55_RZR_FR,
	MALI_C55_RZR_DS,
	MALI_C55_NUM_RZRS,
};

enum mali_c55_rzr_pads {
	MALI_C55_RZR_SINK_PAD,
	MALI_C55_RZR_SOURCE_PAD,
	MALI_C55_RZR_SINK_BYPASS_PAD,
	MALI_C55_RZR_NUM_PADS
};

struct mali_c55_resizer {
	struct mali_c55 *mali_c55;
	struct mali_c55_cap_dev *cap_dev;
	enum mali_c55_resizer_ids id;
	struct v4l2_subdev sd;
	struct media_pad pads[MALI_C55_RZR_NUM_PADS];
	unsigned int num_routes;
};

enum mali_c55_cap_devs {
	MALI_C55_CAP_DEV_FR,
	MALI_C55_CAP_DEV_DS,
	MALI_C55_NUM_CAP_DEVS
};

struct mali_c55_fmt {
	u32 fourcc;
	unsigned int mbus_codes[2];
	bool is_raw;
	bool enumerate;
	struct mali_c55_fmt_registers {
		unsigned int base_mode;
		unsigned int uv_plane;
	} registers;
};

enum mali_c55_isp_bayer_order {
	MALI_C55_BAYER_ORDER_RGGB,
	MALI_C55_BAYER_ORDER_GRBG,
	MALI_C55_BAYER_ORDER_GBRG,
	MALI_C55_BAYER_ORDER_BGGR
};

struct mali_c55_isp_fmt {
	u32 code;
	u8 bitwidth;
	enum v4l2_pixel_encoding encoding;
	enum mali_c55_isp_bayer_order order;
};

enum mali_c55_planes {
	MALI_C55_PLANE_Y,
	MALI_C55_PLANE_UV,
	MALI_C55_NUM_PLANES
};

struct mali_c55_buffer {
	struct vb2_v4l2_buffer vb;
	bool plane_done[MALI_C55_NUM_PLANES];
	struct list_head queue;
	u32 addrs[MALI_C55_NUM_PLANES];
};

struct mali_c55_cap_dev {
	struct mali_c55 *mali_c55;
	struct mali_c55_resizer *rzr;
	struct video_device vdev;
	struct media_pad pad;
	struct vb2_queue queue;
	struct mutex lock;
	unsigned int reg_offset;

	struct mali_c55_mode {
		const struct mali_c55_fmt *capture_fmt;
		struct v4l2_pix_format_mplane pix_mp;
	} mode;

	struct {
		spinlock_t lock;
		struct list_head queue;
		struct mali_c55_buffer *curr;
		struct mali_c55_buffer *next;
		unsigned int framecount;
	} buffers;

	bool streaming;
};

enum mali_c55_config_spaces {
	MALI_C55_CONFIG_PING,
	MALI_C55_CONFIG_PONG,
	MALI_C55_NUM_CONFIG_SPACES
};

struct mali_c55_ctx {
	struct mali_c55 *mali_c55;
	void *registers;
	phys_addr_t base;
	spinlock_t lock;
	struct list_head list;
};

struct mali_c55 {
	struct device *dev;
	struct resource *res;
	void __iomem *base;
	struct dma_chan *channel;
	struct clk_bulk_data clks[ARRAY_SIZE(mali_c55_clk_names)];

	u16 capabilities;
	struct media_device media_dev;
	struct v4l2_device v4l2_dev;
	struct media_pipeline pipe;

	struct mali_c55_tpg tpg;
	struct mali_c55_isp isp;
	struct mali_c55_resizer resizers[MALI_C55_NUM_RZRS];
	struct mali_c55_cap_dev cap_devs[MALI_C55_NUM_CAP_DEVS];

	struct list_head contexts;
	enum mali_c55_config_spaces next_config;
};

void mali_c55_write(struct mali_c55 *mali_c55, unsigned int addr, u32 val);
u32 mali_c55_read(struct mali_c55 *mali_c55, unsigned int addr,
		  bool force_hardware);
void mali_c55_update_bits(struct mali_c55 *mali_c55, unsigned int addr,
			  u32 mask, u32 val);
int mali_c55_config_write(struct mali_c55_ctx *ctx,
			  enum mali_c55_config_spaces cfg_space);

int mali_c55_register_isp(struct mali_c55 *mali_c55);
int mali_c55_register_tpg(struct mali_c55 *mali_c55);
void mali_c55_unregister_tpg(struct mali_c55 *mali_c55);
void mali_c55_unregister_isp(struct mali_c55 *mali_c55);
int mali_c55_register_resizers(struct mali_c55 *mali_c55);
void mali_c55_unregister_resizers(struct mali_c55 *mali_c55);
int mali_c55_register_capture_devs(struct mali_c55 *mali_c55);
void mali_c55_unregister_capture_devs(struct mali_c55 *mali_c55);
struct mali_c55_ctx *mali_c55_get_active_context(struct mali_c55 *mali_c55);
void mali_c55_set_plane_done(struct mali_c55_cap_dev *cap_dev,
			     enum mali_c55_planes plane);
void mali_c55_set_next_buffer(struct mali_c55_cap_dev *cap_dev);

const struct mali_c55_fmt *mali_c55_cap_fmt_next(const struct mali_c55_fmt *fmt,
						 bool allow_raw, bool unique);
bool mali_c55_format_is_raw(unsigned int mbus_code);
void mali_c55_rzr_start_stream(struct mali_c55_resizer *rzr);
int mali_c55_isp_s_stream(struct mali_c55_isp *isp, int enable);
#define for_each_mali_cap_fmt(fmt, raw)\
	for ((fmt) = NULL; ((fmt) = mali_c55_cap_fmt_next((fmt), (raw), false));)
#define for_each_unique_mali_cap_fmt(fmt, raw)\
	for ((fmt) = NULL; ((fmt) = mali_c55_cap_fmt_next((fmt), (raw), true));)

const struct mali_c55_isp_fmt *
mali_c55_isp_fmt_next(const struct mali_c55_isp_fmt *fmt);
bool mali_c55_isp_is_format_supported(unsigned int mbus_code);
#define for_each_mali_isp_fmt(fmt)\
	for ((fmt) = NULL; ((fmt) = mali_c55_isp_fmt_next((fmt)));)

#endif /* _MALI_C55_COMMON_H */
