/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VEPU510 (RK3576) hardware video encoder driver.
 *
 * Copyright (C) 2026 Jiaxing Hu <gahing@gahingwoo.com>
 */

#ifndef RKVENC_H_
#define RKVENC_H_

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>

struct rkvenc_ctx;

struct rkvenc_fmt {
	u32 fourcc;
	unsigned int frmsize_min_width;
	unsigned int frmsize_max_width;
	unsigned int frmsize_step_width;
	unsigned int frmsize_min_height;
	unsigned int frmsize_max_height;
	unsigned int frmsize_step_height;
};

/*
 * Per-coded-format (H.264, later HEVC) operation vtable. Mirrors the split
 * rkvdec uses for its per-codec backends, adapted for a *stateful* mem2mem
 * encoder (no V4L2 request-API preamble/postamble, unlike rkvdec) — the
 * device_run()/done() split matches drivers/media/platform/verisilicon's
 * hantro_codec_ops instead.
 */
struct rkvenc_coded_fmt_ops {
	/* Registers this format's V4L2 controls into ctx->ctrl_handler. */
	int (*init_ctrls)(struct rkvenc_ctx *ctx);
	int (*start)(struct rkvenc_ctx *ctx);
	void (*stop)(struct rkvenc_ctx *ctx);
	void (*run)(struct rkvenc_ctx *ctx);
	/* Called from the threaded IRQ handler once hardware reports frame
	 * done. Must set the CAPTURE buffer's bytesused/timestamp/flags.
	 */
	void (*done)(struct rkvenc_ctx *ctx, enum vb2_buffer_state state);
};

struct rkvenc_coded_fmt_desc {
	u32 fourcc;
	struct rkvenc_fmt frmsize;
	const struct rkvenc_coded_fmt_ops *ops;
};

struct rkvenc_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m_dev;

	struct device *dev;
	void __iomem *regs;
	struct clk_bulk_data *clocks;
	int num_clocks;
	struct reset_control *rst;

	/* Protects ctx list / hardware access ordering. vb2 queues already
	 * serialize per-context streaming; this additionally serializes
	 * access to the one physical core across contexts (rkvenc0/rkvenc1
	 * are two separate rkvenc_dev instances, each owning one core —
	 * v1 does not implement the vendor driver's CCU cross-core task
	 * dispatch/load-balancing).
	 */
	struct mutex vfd_mutex;

	struct rkvenc_ctx *cur_ctx;
	struct delayed_work watchdog_work;

	unsigned int core_clk_rate;

	/* Accumulated by the hard IRQ handler, consumed by the threaded one. */
	u32 irq_status;
};

struct rkvenc_h264_ctx {
	struct v4l2_ctrl *bitrate;
	struct v4l2_ctrl *profile;
	struct v4l2_ctrl *level;
	struct v4l2_ctrl *entropy_mode;
	struct v4l2_ctrl *gop_size;
	struct v4l2_ctrl *i_qp;
	struct v4l2_ctrl *p_qp;

	unsigned int frame_num;
	unsigned int poc_lsb;
	unsigned int gop_pos;
	unsigned int idr_pic_id;

	/* Whether the *previous* encoded frame was an IDR -- mpp's
	 * setup_vepu510_anti_smear() keys smear_opt_cfg.stated_mode off both
	 * the current slice type AND the previous frame's, see rkvenc-regs.h.
	 * Zeroed by rkvenc_h264_start()'s memset; doesn't matter for frame 0
	 * (always IDR regardless).
	 */
	bool last_frame_was_idr;

	/* Software-synthesized SPS/PPS, emitted (Annex-B) ahead of the first
	 * hardware slice payload of each IDR access unit; see rkvenc-h264.c.
	 */
	u8 sps_pps_nal[64];
	size_t sps_pps_len;

	/* VEPU510 reconstructs/references frames only through three internal
	 * scratch regions per frame slot — an FBC-compressed pixel buffer
	 * (header+body), a "thumbnail" downsample buffer (dspw/dspr_addr),
	 * and a "smear" buffer (adr_smear_wr/rd) — all three are written by
	 * hardware on *every* frame regardless of which encoder features are
	 * in use (confirmed from mpp's setup_vepu510_recn_refr(), which sets
	 * all three unconditionally whenever a current/reference buffer slot
	 * exists at all). Leaving any of them at a NULL pointer causes a
	 * continuous IOMMU write-fault storm at iova 0x0, not a clean error.
	 *
	 * This driver owns a 2-deep ping-pong pool of driver-private DMA
	 * buffers: buf[frame_num % 2] is this frame's write target,
	 * buf[(frame_num + 1) % 2] is the previous frame's read reference —
	 * EXCEPT on the very first frame of a session (frame_num == 0), where
	 * there is no previously-written reference at all: mpp's real
	 * hal_h264e_vepu510.c setup_vepu510_recn_refr() draws the "read" side
	 * from a `refr` ping-pong slot that equals the `curr` (write) slot
	 * index for frame 0, i.e. real hardware gets read == write, the same
	 * buffer aliased both ways, not a second never-written allocation.
	 * See rkvenc_h264_run()'s read_idx computation.
	 *
	 * Each buffer is one dma_alloc_coherent() allocation split into three
	 * consecutive regions [0, pixel_buf_size), [pixel_buf_size,
	 * +thumb_buf_size), [+thumb_buf_size, +smear_buf_size). Sizes are
	 * mpp's exact setup_vepu510_prepare_recn_bufs() formulas, not a guess.
	 */
	void *recn_buf_cpu[2];
	dma_addr_t recn_buf_dma[2];
	size_t pixel_buf_size;	/* FBC header + body */
	size_t pixel_hdr_size;	/* FBC header portion of pixel_buf_size */
	size_t thumb_buf_size;
	size_t smear_buf_size;

	/* Motion-detection-info write buffer (meiw_addr). Real hardware
	 * writes here even though nothing in the userspace HAL documents the
	 * feature as required for a plain encode — board bring-up found a
	 * real successful encode always has a real address here, so this
	 * driver allocates one rather than assuming 0 is safe. Content is
	 * never read back (v1 doesn't expose motion vectors to userspace).
	 */
	void *meiw_buf_cpu;
	dma_addr_t meiw_buf_dma;
};

struct rkvenc_ctx {
	struct v4l2_fh fh;
	struct rkvenc_dev *dev;

	struct v4l2_pix_format src_fmt;	/* OUTPUT: raw NV12 */
	struct v4l2_pix_format coded_fmt; /* CAPTURE: H264/HEVC */
	const struct rkvenc_coded_fmt_desc *coded_desc;

	struct v4l2_ctrl_handler ctrl_handler;

	union {
		struct rkvenc_h264_ctx h264;
	};
};

static inline struct rkvenc_ctx *fh_to_rkvenc_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct rkvenc_ctx, fh);
}

static inline u32 rkvenc_read(struct rkvenc_dev *dev, u32 reg)
{
	return readl(dev->regs + reg);
}

static inline void rkvenc_write(struct rkvenc_dev *dev, u32 reg, u32 val)
{
	writel(val, dev->regs + reg);
}

static inline void rkvenc_write_relaxed(struct rkvenc_dev *dev, u32 reg, u32 val)
{
	writel_relaxed(val, dev->regs + reg);
}

static inline struct vb2_v4l2_buffer *rkvenc_get_src_buf(struct rkvenc_ctx *ctx)
{
	return v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
}

static inline struct vb2_v4l2_buffer *rkvenc_get_dst_buf(struct rkvenc_ctx *ctx)
{
	return v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
}

extern const struct rkvenc_coded_fmt_desc rkvenc_h264_fmt_desc;

void rkvenc_job_finish(struct rkvenc_dev *dev, struct rkvenc_ctx *ctx,
		       enum vb2_buffer_state state);
u32 rkvenc_calc_wdg(struct rkvenc_dev *dev, unsigned int width,
		    unsigned int height);

#endif /* RKVENC_H_ */
