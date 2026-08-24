/* SPDX-License-Identifier: (GPL-2.0-or-later OR MIT) */
/*
 * Rockchip ISP2 Driver - Common definitions
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 *
 * Based on Rockchip ISP2 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#ifndef _RKISP2_COMMON_H
#define _RKISP2_COMMON_H

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rkisp2-config.h>
#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-isp.h>
#include <media/videobuf2-v4l2.h>

#include "rkisp2-regs.h"

struct dentry;
struct regmap;

/*
 * flags on the 'direction' field in struct rkisp2_mbus_info' that indicate
 * on which pad the media bus format is supported
 */
#define RKISP2_ISP_SD_SRC			BIT(0)
#define RKISP2_ISP_SD_SINK			BIT(1)

/*
 * Minimum values for the width and height of entities. The maximum values are
 * model-specific and stored in the rkisp2_info structure.
 */
#define RKISP2_ISP_MIN_WIDTH			32
#define RKISP2_ISP_MIN_HEIGHT			32

#define RKISP2_RSZ_MP_SRC_MAX_WIDTH		4416
#define RKISP2_RSZ_MP_SRC_MAX_HEIGHT		3312
#define RKISP2_RSZ_SP_SRC_MAX_WIDTH		1920
#define RKISP2_RSZ_SP_SRC_MAX_HEIGHT		1920
#define RKISP2_RSZ_SRC_MIN_WIDTH		32
#define RKISP2_RSZ_SRC_MIN_HEIGHT		16

/* the default width and height of all the entities */
#define RKISP2_DEFAULT_WIDTH			800
#define RKISP2_DEFAULT_HEIGHT			600

#define RKISP2_DRIVER_NAME			"rkisp2"
#define RKISP2_BUS_INFO				"platform:" RKISP2_DRIVER_NAME

/* maximum number of clocks */
#define RKISP2_MAX_BUS_CLK			8

/* IRQ lines */
enum rkisp2_irq_line {
	RKISP2_IRQ_ISP = 0,
	RKISP2_IRQ_MI,
	RKISP2_IRQ_MIPI,
	RKISP2_NUM_IRQS,
};

/* enum for the resizer pads */
/* enum for the csi receiver pads */
/* enum for the capture id */
enum rkisp2_stream_id {
	RKISP2_MAINPATH,
	RKISP2_SELFPATH,
	RKISP2_MAINPATH_FBC = 31,
};

/* bayer patterns */
enum rkisp2_fmt_raw_pat_type {
	RKISP2_RAW_RGGB = 0,
	RKISP2_RAW_GRBG,
	RKISP2_RAW_GBRG,
	RKISP2_RAW_BGGR,
};

/* This needs to match the sink video pads below */
enum rkisp2_rawrd_id {
	RKISP2_RAWRD0,
	RKISP2_RAWRD1,
	RKISP2_RAWRD2,
	RKISP2_RAWRD_MAX,
};

/* enum for the isp pads */
enum rkisp2_isp_pad {
	RKISP2_ISP_PAD_SINK_VIDEO_DMA_BASE,
	RKISP2_ISP_PAD_SINK_VIDEO_DMA_0 = RKISP2_ISP_PAD_SINK_VIDEO_DMA_BASE,
	RKISP2_ISP_PAD_SINK_VIDEO_DMA_1,
	RKISP2_ISP_PAD_SINK_VIDEO_DMA_2,
	RKISP2_ISP_PAD_SINK_VIDEO_DMA_MAX,
	RKISP2_ISP_PAD_SINK_VIDEO_CIF = RKISP2_ISP_PAD_SINK_VIDEO_DMA_MAX,
	RKISP2_ISP_PAD_SINK_PARAMS,
	RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN,
	RKISP2_ISP_PAD_SOURCE_VIDEO_SELF,
	RKISP2_ISP_PAD_SOURCE_STATS,
	RKISP2_ISP_PAD_MAX
};

/*
 * enum rkisp2_feature - ISP features
 *
 * @RKISP2_FEATURE_DUAL_CROP: The ISP has the dual crop block at the resizer input
 *
 * The ISP features are stored in a bitmask in &rkisp2_info.features and allow
 * the driver to implement support for features present in some ISP versions
 * only.
 */
enum rkisp2_feature {
	RKISP2_FEATURE_DUAL_CROP = BIT(0),
};

#define rkisp2_has_feature(rkisp2, feature) \
	((rkisp2)->info->features & RKISP2_FEATURE_##feature)

/*
 * struct rkisp2_info - Model-specific ISP Information
 *
 * @clks: array of ISP clock names
 * @clk_size: number of entries in the @clks array
 * @isrs: array of ISP interrupt descriptors
 * @isr_size: number of entries in the @isrs array
 * @isp_ver: ISP version
 * @features: bitmask of rkisp2_feature features implemented by the ISP
 * @max_width: maximum input frame width
 * @max_height: maximum input frame height
 *
 * This structure contains information about the ISP specific to a particular
 * ISP model, version, or integration in a particular SoC.
 */
struct rkisp2_info {
	const char * const *clks;
	unsigned int clk_size;
	const struct rkisp2_isr_data *isrs;
	unsigned int isr_size;
	enum rkisp2_isp_version isp_ver;
	unsigned int features;
	unsigned int max_width;
	unsigned int max_height;
};

/*
 * struct rkisp2_isp - ISP subdev entity
 *
 * @sd:				v4l2_subdev variable
 * @rkisp2:			pointer to rkisp2_device
 * @pads:			media pads
 * @sink_fmt:			input format
 * @frame_sequence:		used to synchronize frame_id between video devices.
 * @dphy_errctrl_disabled:	flag to re-enable DPHY errctrl interrupt after error
 */
struct rkisp2_isp {
	struct v4l2_subdev sd;
	struct rkisp2_device *rkisp2;
	struct media_pad pads[RKISP2_ISP_PAD_MAX];
	const struct rkisp2_mbus_info *sink_fmt;
	__u32 frame_sequence;
	bool dphy_errctrl_disabled;
};

/*
 * struct rkisp2_vdev_node - Container for the video nodes: params, stats, mainpath, selfpath
 *
 * @buf_queue:	queue of buffers
 * @vlock:	lock of the video node
 * @vdev:	video node
 * @pad:	media pad
 */
struct rkisp2_vdev_node {
	struct vb2_queue buf_queue;
	struct mutex vlock; /* ioctl serialization mutex */
	struct video_device vdev;
	struct media_pad pad;
};

/*
 * struct rkisp2_buffer - A container for the vb2 buffers used by the video devices:
 *			  stats, mainpath, selfpath
 *
 * @vb:		vb2 buffer
 * @queue:	entry of the buffer in the queue
 * @buff_addr:	dma addresses of each plane, used only by the capture devices: selfpath, mainpath
 */
struct rkisp2_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	dma_addr_t buff_addr[VIDEO_MAX_PLANES];
};

/*
 * struct rkisp2_params_buffer - A container for the vb2 buffers used by the
 *				 params video device
 *
 * @vb:		vb2 buffer
 * @queue:	entry of the buffer in the queue
 * @cfg:	scratch buffer used for caching the ISP configuration parameters
 */
struct rkisp2_params_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
	struct v4l2_isp_params_buffer *cfg;
};

static inline struct rkisp2_params_buffer *
to_rkisp2_params_buffer(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct rkisp2_params_buffer, vb);
}

/*
 * struct rkisp2_dummy_buffer - A buffer to write the next frame to in case
 *				there are no vb2 buffers available.
 *
 * @vaddr:	return value of call to dma_alloc_attrs.
 * @dma_addr:	dma address of the buffer.
 * @size:	size of the buffer.
 */
struct rkisp2_dummy_buffer {
	void *vaddr;
	dma_addr_t dma_addr;
	u32 size;
};

struct rkisp2_device;

struct rkisp2_dmarx_fmt {
	u32 fourcc;
	u8 bpp;
};

struct rkisp2_rawrd_cfg {
	const char *name;
	u32 ready_mask;
	u32 base_reg;
	u32 length_reg;
	u32 enable_mask;
	u8 channel_sel;
};

struct rkisp2_dmarx_chan {
	struct rkisp2_device *rkisp2;
	struct rkisp2_vdev_node vnode;
	spinlock_t buf_lock;
	struct list_head buf_queue;
	struct rkisp2_buffer *curr_buf;
	const struct rkisp2_dmarx_fmt *fmts;
	unsigned int fmt_cnt;
	const struct rkisp2_dmarx_fmt *fmt;
	struct v4l2_pix_format_mplane pix;
	const struct rkisp2_rawrd_cfg *cfg;
	enum rkisp2_rawrd_id id;
	bool streaming;
};

struct rkisp2_dmarx {
	struct rkisp2_dmarx_chan chan[RKISP2_RAWRD_MAX];
	unsigned int num_chans;
};

/*
 * struct rkisp2_capture - ISP capture video device
 *
 * @vnode:	  video node
 * @rkisp2:	  pointer to rkisp2_device
 * @id:		  id of the capture, one of RKISP2_SELFPATH, RKISP2_MAINPATH
 * @ops:	  list of callbacks to configure the capture device.
 * @config:	  a pointer to the list of registers to configure the capture format.
 * @is_streaming: device is streaming
 * @is_stopping:  stop_streaming callback was called and the device is in the process of
 *		  stopping the streaming.
 * @done:	  when stop_streaming callback is called, the device waits for the next irq
 *		  handler to stop the streaming by waiting on the 'done' wait queue.
 *		  If the irq handler is not called, the stream is stopped by the callback
 *		  after timeout.
 * @stride:       the line stride for the first plane, in pixel units
 * @buf.lock:	  lock to protect buf.queue
 * @buf.queue:	  queued buffer list
 * @buf.dummy:	  dummy space to store dropped data
 *
 * rkisp2 uses shadow registers, so it needs two buffers at a time
 * @buf.curr:	  the buffer used for current frame
 * @buf.next:	  the buffer used for next frame
 * @pix.cfg:	  pixel configuration
 * @pix.info:	  a pointer to the v4l2_format_info of the pixel format
 * @pix.fmt:	  buffer format
 */
struct rkisp2_capture {
	struct rkisp2_vdev_node vnode;
	struct rkisp2_device *rkisp2;
	enum rkisp2_stream_id id;
	const struct rkisp2_capture_ops *ops;
	const struct rkisp2_capture_config *config;
	bool is_streaming;
	bool is_stopping;
	wait_queue_head_t done;
	unsigned int stride;
	struct {
		/* protects queue, curr and next */
		spinlock_t lock;
		struct list_head queue;
		struct rkisp2_dummy_buffer dummy;
		struct rkisp2_buffer *curr;
		struct rkisp2_buffer *next;
	} buf;
	struct {
		const struct rkisp2_capture_fmt_cfg *cfg;
		const struct v4l2_format_info *info;
		struct v4l2_pix_format_mplane fmt;
	} pix;
};

/*
 * struct rkisp2_stats - ISP Statistics device
 *
 * @vnode:	  video node
 * @rkisp2:	  pointer to the rkisp2 device
 * @lock:	  locks the buffer list 'stat'
 * @stat:	  queue of rkisp2_buffer
 * @vdev_fmt:	  v4l2_format of the metadata format
 * @icr:	  interrupts that the stats has handled for the current frame (for stats 3a)
 * @cur_buf:	  the current buffer to accumulate stats over multiple interrupts
 *
 * The icr field tracks which stats have been handled and whose interrupts will
 * be cleared at end-of-frame when the stats are being handled.
 *
 * The stats are handled at end-of-frame as opposed to on the stats3a
 * interrupts as there is one interrupt per stats block that has completed, and
 * as we don't yet support returning partial stats to userspace, it's not
 * really worth accumulating them separately. Thus we can simply accumulate
 * whichever stats blocks have completed by checking their respective done bits
 * (except ae_lite, whose bit is never set) at end-of-frame time.
 *
 * A note on the per-block stats3a interrupts: There seem to be separate
 * interrupts in stats 3a for big modules and non-big modules. The conditions
 * that cause separate interrupts are not yet fully understood, but is
 * hypothesized to be big-mode & different window size compared to lite mode.
 * The core stats (in the main ISP register space, similar to the rkisp1) are
 * hypothesized to not exist.
 */
struct rkisp2_stats {
	struct rkisp2_vdev_node vnode;
	struct rkisp2_device *rkisp2;

	spinlock_t lock; /* locks the buffers list 'stats' */
	struct list_head stat;
	struct v4l2_format vdev_fmt;

	u32 icr;
	struct rkisp2_buffer *cur_buf;

	unsigned int awb_window_offset;
};

/*
 * struct rkisp2_params - ISP input parameters device
 *
 * @vnode:			video node
 * @rkisp2:			pointer to the rkisp2 device
 * @buf_lock:			locks the buffer list 'params'
 * @params:			queue of rkisp2_buffer
 * @raw_type:			the bayer pattern on the isp video sink pad
 */
struct rkisp2_params {
	struct rkisp2_vdev_node vnode;
	struct rkisp2_device *rkisp2;

	spinlock_t buf_lock; /* locks the buffers list 'params' */
	struct list_head params;

	enum rkisp2_fmt_raw_pat_type raw_type;
};

/*
 * struct rkisp2_resizer - ISP resizer device
 *
 * This struct stores information relevant to the configuration of the resizer
 * and dual crop blocks. There is no associated subdev because hardware-wise
 * this resides in the ISP block, but we configure it via parameters buffers to
 * enable synchronous crop rectangle configuration. The configuration
 * information is populated at s_stream time and does not change.
 *
 * @rkisp2:			Pointer to the rkisp2 device
 * @id:				id of the capture, one of RKISP2_SELFPATH, RKISP2_MAINPATH
 * @config:			The register set for the resizer and dual crop
 * @sink_size:			The size of the sink format of the ISP subdev sink pad
 * @source_size:		The size of the pad-specific source format of the ISP subdev sink pad
 * @source_format:		The mbus format of the ISP subdev source pad
 * @source_fmt_info:		A pointer to the v4l2_format_info of the pixel
 *
 *
 * @sink_size is the ISP sink size (corresponding to the output of IS). It will
 * be used to compare the crop rectangle to to check if dual crop needs to be
 * enabled or not.
 *
 * @source_size is the output of the combined dual crop + resizer.
 */
struct rkisp2_resizer {
	struct rkisp2_device *rkisp2;
	enum rkisp2_stream_id id;
	const struct rkisp2_resizer_config *config;

	struct v4l2_rect sink_size;
	struct v4l2_rect source_size;

	struct v4l2_mbus_framefmt source_format;
	const struct v4l2_format_info *source_fmt_info;
};

struct rkisp2_debug {
	struct dentry *debugfs_dir;
	unsigned long data_loss;
	unsigned long outform_size_error;
	unsigned long img_stabilization_size_error;
	unsigned long inform_size_error;
	unsigned long irq_delay;
	unsigned long mipi_error;
	unsigned long stats_error;
	unsigned long stats3a_irq;
	unsigned long stop_timeout[2];
	unsigned long frame_drop[2];
	unsigned long complete_frames;
	unsigned long stats3a_hist_ch0_count;
	unsigned long stats3a_hist_ch1_count;
	unsigned long stats3a_hist_ch2_count;
	unsigned long stats3a_hist_big_count;
	unsigned long stats3a_awb_count;
	unsigned long stats3a_awb_done_count;
};

/*
 * struct rkisp2_device - ISP platform device
 *
 * @base_addr:	   base register address
 * @dev:	   a pointer to the struct device
 * @clk_size:	   number of clocks
 * @clks:	   array of clocks
 * @gasket:	   the gasket - i.MX8MP only
 * @gasket_id:	   the gasket ID (0 or 1) - i.MX8MP only
 * @v4l2_dev:	   v4l2_device variable
 * @media_dev:	   media_device variable
 * @notifier:	   a notifier to register on the v4l2-async API to be notified on the sensor
 * @source:        source subdev in-use, set when starting streaming
 * @csi:	   internal CSI-2 receiver
 * @isp:	   ISP sub-device
 * @resizer_devs:  resizer sub-devices
 * @capture_devs:  capture devices
 * @dmarx:	   ISP memory read device
 * @params:	   ISP parameters metadata output device
 * @stats:	   ISP statistics metadata capture device
 * @pipe:	   media pipeline
 * @stream_lock:   serializes {start/stop}_streaming callbacks between the capture devices.
 * @debug:	   debug params to be exposed on debugfs
 * @info:	   version-specific ISP information
 * @irqs:          IRQ line numbers
 * @irqs_enabled:  the hardware is enabled and can cause interrupts
 */
struct rkisp2_device {
	void __iomem *base_addr;
	struct device *dev;
	unsigned int clk_size;
	struct clk_bulk_data clks[RKISP2_MAX_BUS_CLK];
	struct regmap *gasket;
	unsigned int gasket_id;
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *source;
	struct rkisp2_isp isp;
	struct rkisp2_resizer resizer_devs[2];
	struct rkisp2_capture capture_devs[2];
	struct rkisp2_dmarx dmarx;
	struct rkisp2_params params;
	struct rkisp2_stats stats;
	struct media_pipeline pipe;
	struct mutex stream_lock; /* serialize {start/stop}_streaming cb between capture devices */
	struct rkisp2_debug debug;
	const struct rkisp2_info *info;
	int irqs[RKISP2_NUM_IRQS];
	bool irqs_enabled;
};

/*
 * struct rkisp2_mbus_info - ISP media bus info, Translates media bus code to hardware
 *			     format values
 *
 * @mbus_code: media bus code
 * @pixel_enc: pixel encoding
 * @mipi_dt:   mipi data type
 * @yuv_seq:   the order of the Y, Cb, Cr values
 * @bus_width: bus width
 * @bayer_pat: bayer pattern
 * @direction: a bitmask of the flags indicating on which pad the format is supported on
 */
struct rkisp2_mbus_info {
	u32 mbus_code;
	enum v4l2_pixel_encoding pixel_enc;
	u32 mipi_dt;
	u32 yuv_seq;
	u8 bus_width;
	enum rkisp2_fmt_raw_pat_type bayer_pat;
	unsigned int direction;
};

static inline void
rkisp2_write(struct rkisp2_device *rkisp2, unsigned int addr, u32 val)
{
	writel(val, rkisp2->base_addr + addr);
}

static inline u32 rkisp2_read(struct rkisp2_device *rkisp2, unsigned int addr)
{
	return readl(rkisp2->base_addr + addr);
}

/*
 * rkisp2_cap_enum_mbus_codes - A helper function that return the i'th supported mbus code
 *				of the capture entity. This is used to enumerate the supported
 *				mbus codes on the source pad of the resizer.
 *
 * @cap:  the capture entity
 * @code: the mbus code, the function reads the code->index and fills the code->code
 */
int rkisp2_cap_enum_mbus_codes(struct rkisp2_capture *cap,
			       struct v4l2_subdev_mbus_code_enum *code);

/*
 * rkisp2_mbus_info_get_by_index - Retrieve the ith supported mbus info
 *
 * @index: index of the mbus info to fetch
 */
const struct rkisp2_mbus_info *rkisp2_mbus_info_get_by_index(unsigned int index);

/*
 * rkisp2_sd_adjust_crop_rect - adjust a rectangle to fit into another rectangle.
 *
 * @crop:   rectangle to adjust.
 * @bounds: rectangle used as bounds.
 */
void rkisp2_sd_adjust_crop_rect(struct v4l2_rect *crop,
				const struct v4l2_rect *bounds);

/*
 * rkisp2_sd_adjust_crop - adjust a rectangle to fit into media bus format
 *
 * @crop:   rectangle to adjust.
 * @bounds: media bus format used as bounds.
 */
void rkisp2_sd_adjust_crop(struct v4l2_rect *crop,
			   const struct v4l2_mbus_framefmt *bounds);

/*
 * rkisp2_mbus_info_get_by_code - get the isp info of the media bus code
 *
 * @mbus_code: the media bus code
 */
const struct rkisp2_mbus_info *rkisp2_mbus_info_get_by_code(u32 mbus_code);

/* irq handlers */
irqreturn_t rkisp2_isp_isr(int irq, void *ctx);
irqreturn_t rkisp2_capture_isr(int irq, void *ctx);
irqreturn_t rkisp2_mipi_isr(int irq, void *ctx);
void rkisp2_dmarx_isr(struct rkisp2_device *rkisp2, u32 status);
void rkisp2_params_isr(struct rkisp2_params *params);
irqreturn_t rkisp2_stats_isr_eof(struct rkisp2_stats *);

/* register/unregisters functions of the entities */
int rkisp2_capture_devs_register(struct rkisp2_device *rkisp2);
void rkisp2_capture_devs_unregister(struct rkisp2_device *rkisp2);

int rkisp2_isp_register(struct rkisp2_device *rkisp2);
void rkisp2_isp_unregister(struct rkisp2_device *rkisp2);
int rkisp2_dmarx_register(struct rkisp2_device *rkisp2);
void rkisp2_dmarx_unregister(struct rkisp2_device *rkisp2);

int rkisp2_params_register(struct rkisp2_device *rkisp2);
void rkisp2_params_unregister(struct rkisp2_device *rkisp2);
void rkisp2_params_pre_configure(struct rkisp2_params *params,
				 enum rkisp2_fmt_raw_pat_type bayer_pat,
				 const struct v4l2_mbus_framefmt *sink_frm,
				 const struct v4l2_mbus_framefmt *mp_src_frm,
				 const struct v4l2_mbus_framefmt *sp_src_frm);
void rkisp2_params_post_configure(struct rkisp2_params *params);

void rkisp2_resizer_configure(struct rkisp2_resizer *rsz,
			      struct v4l2_rect *crop);
void rkisp2_resizer_pre_configure(struct rkisp2_device *rkisp2,
				  const struct v4l2_mbus_framefmt *sink_frm,
				  const struct v4l2_mbus_framefmt *mp_src_frm,
				  const struct v4l2_mbus_framefmt *sp_src_frm);
void rkisp2_resizer_devs_init(struct rkisp2_device *rkisp2);

int rkisp2_stats_register(struct rkisp2_device *rkisp2);
void rkisp2_stats_unregister(struct rkisp2_device *rkisp2);

#if IS_ENABLED(CONFIG_DEBUG_FS)
void rkisp2_debug_init(struct rkisp2_device *rkisp2);
void rkisp2_debug_cleanup(struct rkisp2_device *rkisp2);
#else
static inline void rkisp2_debug_init(struct rkisp2_device *rkisp2)
{
}
static inline void rkisp2_debug_cleanup(struct rkisp2_device *rkisp2)
{
}
#endif

#endif /* _RKISP2_COMMON_H */
