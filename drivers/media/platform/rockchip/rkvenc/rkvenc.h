/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Rockchip RKVENC v1 H.264 / HEVC encoder driver.
 *
 * struct rkvenc_frame carries no V4L2 types, so that a stateless front end
 * can drive the same hardware back end.
 */

#ifndef RKVENC_H
#define RKVENC_H

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/reset.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>

#include "rkvenc-vepu541.h"

#define RKVENC_HDR_MAX		1024

#define RKVENC_SLOTS		2
#define RKVENC_SLOT_BUFS 4

#define RKVENC_NUM_CLKS		3
#define RKVENC_NUM_RESETS	3

#define RKVENC_MAX_CONSEC_ERRORS	2

struct rkvenc_dev;

/**
 * struct rkvenc_variant - per-SoC integration differences.
 * @vepu540: RK3566/RK3568 "vepu540" rather than RV1109/RV1126 "vepu541".
 */
struct rkvenc_variant {
	bool vepu540;
};

struct rkvenc_dev {
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct device *dev;
	void __iomem *regs;

	struct clk_bulk_data clocks[RKVENC_NUM_CLKS];
	int num_clocks;
	struct reset_control_bulk_data resets[RKVENC_NUM_RESETS];
	int num_resets;

	const struct rkvenc_variant *variant;

	struct rkvenc_img img;

	struct mutex vdev_lock;		/* serialises the video node ioctls */
	spinlock_t irq_lock;
	struct completion sclr_done;
	u32 irq_sta;
	struct delayed_work watchdog;
	bool job_running;
	struct rkvenc_ctx *job_ctx;
	u32 max_width;
	u32 max_height;
};

struct rkvenc_ctrls {
	u32 bitrate_mode;
	u32 bitrate;
	u32 vbv_size;
	bool frame_rc_enable;
	u32 gop_size;
	u32 i_period;
	u32 header_mode;
	bool prepend_spspps;
	bool force_keyframe;

	u8 profile;
	u8 level;
	u8 tier;
	bool cabac;
	bool transform_8x8;
	u8 lf_mode;
	s8 lf_alpha;
	s8 lf_beta;
	s8 hevc_beta_offset;
	s8 hevc_tc_offset;
	s8 chroma_qp_offset;
	u8 min_qp;
	u8 max_qp;
	u8 i_frame_qp;
	u8 p_frame_qp;

	u8 hevc_max_merge;
	bool hevc_sao;
	bool hevc_strong_intra;
	bool hevc_tmv;
	bool hevc_cabac_init;

	u32 framerate;
};

struct rkvenc_gop {
	u32 frames_since_idr;
	u32 idr_pic_id;
	u16 frame_num;
	u16 poc_lsb;
};

struct rkvenc_ctx {
	struct v4l2_fh fh;
	struct rkvenc_dev *dev;
	struct v4l2_m2m_ctx *m2m_ctx;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct mutex vlock;

	u32 src_fourcc;
	u32 coded_fourcc;
	enum rkvenc_codec codec;
	u32 width;
	u32 height;
	u32 bytesperline;
	u32 src_cfmt;

	struct rkvenc_ctrls c;

	struct rkvenc_gop gop;
	struct rkvenc_slot slot[RKVENC_SLOTS];
	struct rkvenc_slot_sizes slot_sz;
	void *slot_cpu[RKVENC_SLOTS][RKVENC_SLOT_BUFS];
	bool slots_alloc;
	int cur;
	int last;
	bool last_good_valid;
	int last_good;
	u32 consecutive_errors;
	bool overflow_retried;

	u32 src_num_planes;

	bool joined_header_done;

	s32 rc_buf; /* in bits */
	u32 rc_tavg;
	s8 rc_qp[2]; /* indexed by frame type, 0 = I, 1 = P */
	u32 rc_x[2];
	s32 rc_buf_gop;
	u32 rc_p_qp_sum;
	u32 rc_p_qp_cnt;
	bool rc_active;
	bool rc_first[2];

	bool header_pending;
	u8 hdr[RKVENC_HDR_MAX];
	u32 hdr_len;

	u32 fps;
	u32 blocks;
	u32 qp_blocks;

	bool cur_intra;
	u8 cur_qp;
	u32 cur_prepend;

	struct rkvenc_h264_seq h264_seq;
	struct rkvenc_hevc_seq hevc_seq;
};

/* rkvenc.c */
int rkvenc_hw_run(struct rkvenc_dev *dev, const struct rkvenc_frame *f);
void rkvenc_hw_status_read(struct rkvenc_dev *dev, u32 *int_sta, u32 *bytes,
			   u32 *qp_sum);
void rkvenc_hw_recover(struct rkvenc_dev *dev);
int rkvenc_v4l2_init(struct rkvenc_dev *dev);
void rkvenc_v4l2_cleanup(struct rkvenc_dev *dev);

/* rkvenc-v4l2.c */
extern const struct v4l2_m2m_ops rkvenc_m2m_ops;

/* rkvenc-gop.c */
void rkvenc_gop_init(struct rkvenc_ctx *ctx);
bool rkvenc_gop_is_idr(struct rkvenc_ctx *ctx);
void rkvenc_gop_commit(struct rkvenc_ctx *ctx);
void rkvenc_gop_rollback(struct rkvenc_ctx *ctx);
void rkvenc_gop_force_keyframe(struct rkvenc_ctx *ctx);

/* rkvenc-ref.c */
int rkvenc_ref_alloc(struct rkvenc_ctx *ctx);
void rkvenc_ref_free(struct rkvenc_ctx *ctx);

/* rkvenc-rc.c */
void rkvenc_rc_init(struct rkvenc_ctx *ctx);
void rkvenc_rc_pick(struct rkvenc_ctx *ctx, u8 *qp, u8 frame_type);
void rkvenc_rc_update(struct rkvenc_ctx *ctx, u32 bytes, u32 qp_avg,
		      u8 frame_type);
void rkvenc_rc_overflow(struct rkvenc_ctx *ctx, u32 bytes, u8 frame_type);

/* rkvenc-h264.c / rkvenc-hevc.c */
int rkvenc_h264_setup(struct rkvenc_ctx *ctx);
int rkvenc_h264_write_headers(struct rkvenc_ctx *ctx, u8 *buf, u32 size);
void rkvenc_h264_build_pic(struct rkvenc_ctx *ctx,
			    struct rkvenc_h264_pic *pic, bool idr);

int rkvenc_hevc_setup(struct rkvenc_ctx *ctx);
int rkvenc_hevc_write_headers(struct rkvenc_ctx *ctx, u8 *buf, u32 size);
void rkvenc_hevc_build_pic(struct rkvenc_ctx *ctx,
			    struct rkvenc_hevc_pic *pic, bool idr);

#endif /* RKVENC_H */
