/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RKVENC_VEPU541_H
#define RKVENC_VEPU541_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/math.h>
#include <linux/log2.h>

#include "rkvenc-vepu541-regs.h"

enum rkvenc_codec {
	RKVENC_CODEC_H264,
	RKVENC_CODEC_HEVC,
};

struct rkvenc_src {
	u32 plane[3];
	u32 stride[2];
	u32 width;
	u32 height;
};

struct rkvenc_bs {
	u32 base;
	u32 size;
	u32 offset;
};

struct rkvenc_slot {
	u32 rfp_hdr;
	u32 rfp_body;
	u32 dsp;
	u32 cmv;
};

struct rkvenc_slot_sizes {
	u32 rfp_hdr;
	u32 rfp_body;
	u32 dsp;
	u32 cmv;
};

struct rkvenc_h264_seq {
	u32 width;
	u32 height;
	u8 profile_idc;
	u8 level_idc;
	u8 log2_max_frame_num_m4;
	u8 log2_max_poc_lsb_m4;
	u8 pic_init_qp;
	s8 chroma_qp_offset;
	bool cabac;
	bool transform_8x8;
	u8 dblk_idc;
	s8 alpha_offset;
	s8 beta_offset;
	bool timing;
	u32 fps;
};

struct rkvenc_h264_pic {
	const struct rkvenc_h264_seq *seq;
	u16 frame_num;
	u16 poc_lsb;
	u16 idr_pic_id;
	u8 nal_ref_idc;
};

struct rkvenc_hevc_seq {
	u32 width;
	u32 height;
	u8 level_idc;
	u8 log2_max_poc_lsb_m4;
	u8 pic_init_qp;
	u8 max_merge_cand;
	bool sao;
	bool deblock_disabled;
	s8 beta_offset;
	s8 tc_offset;
	bool strong_intra_smoothing;
	bool cabac_init_present;
};

struct rkvenc_hevc_pic {
	const struct rkvenc_hevc_seq *seq;
	u16 poc_lsb;
	u8 nal_unit_type;
};

struct rkvenc_rc {
	u32 target_bits;
	u8 qp_range;
	s8 step[RKVENC_RC_STEPS];
};

struct rkvenc_frame {
	enum rkvenc_codec codec;
	bool intra;
	bool referenced;
	struct rkvenc_src src;
	struct rkvenc_bs bs;
	struct rkvenc_slot recon;
	struct rkvenc_slot ref;
	u8 qp;
	u8 qp_min;
	u8 qp_max;
	bool aq;
	const struct rkvenc_rc *rc;
	u32 wdg_vs_load;
	union {
		struct rkvenc_h264_pic h264;
		struct rkvenc_hevc_pic hevc;
	};
};

#define RKVENC_L1_CFG_WORDS		(RKVENC_L1_CFG_END / 4)

struct rkvenc_img {
	u32 l1[RKVENC_L1_CFG_WORDS];
	u32 l2[RKVENC_L2_WORDS];
	u32 l2_last;
	u32 enc_strt;
};

static inline void rkvenc_img_l1(struct rkvenc_img *img, u32 reg, u32 val)
{
	img->l1[reg / 4] = val;
}

static inline void rkvenc_img_l2(struct rkvenc_img *img, u32 reg, u32 val)
{
	img->l2[reg / 4] = val;
}

static inline u32 rkvenc_s2u(s32 val, u32 bits)
{
	return (u32)val & GENMASK(bits - 1, 0);
}

static inline u32 rkvenc_pair(u32 lo, u32 hi)
{
	return lo | hi << 16;
}

static inline u32 rkvenc_bytes(u8 b0, u8 b1, u8 b2, u8 b3)
{
	return b0 | b1 << 8 | b2 << 16 | (u32)b3 << 24;
}

static inline void rkvenc_img_l2_u8(struct rkvenc_img *img, u32 reg,
				    const u8 *v, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		img->l2[reg / 4 + i / 4] |= (u32)v[i] << (8 * (i % 4));
}

static inline void rkvenc_img_l2_s6(struct rkvenc_img *img, u32 reg,
				    const s8 *v, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		img->l2[reg / 4 + i / 4] |= rkvenc_s2u(v[i], 6) << (8 * (i % 4));
}

bool rkvenc_vepu541_l1_written(u32 reg);
void rkvenc_vepu541_slot_sizes(enum rkvenc_codec codec, u32 width, u32 height,
			       struct rkvenc_slot_sizes *sz);
int rkvenc_vepu541_img_build(const struct rkvenc_frame *f,
			     struct rkvenc_img *img);

void rkvenc_vepu541_h264_build(const struct rkvenc_frame *f,
			       struct rkvenc_img *img);
void rkvenc_vepu541_hevc_build(const struct rkvenc_frame *f,
			       struct rkvenc_img *img);

void rkvenc_tables_init(void);
extern u32 rkvenc_h264_klut[RKVENC_KLUT_WORDS];
extern u32 rkvenc_hevc_klut[RKVENC_KLUT_WORDS];
extern u32 rkvenc_h264_lambda[RKVENC_QP_MAX + 2];
extern u32 rkvenc_hevc_lambda_satd[RKVENC_QP_MAX + 1];
extern u32 rkvenc_hevc_lambda_a[RKVENC_QP_MAX + 1];
extern u32 rkvenc_hevc_lambda_b[RKVENC_QP_MAX + 1];
extern const u8 rkvenc_h264_aq_thd[16];
extern const s8 rkvenc_h264_aq_step_i[16];
extern const s8 rkvenc_h264_aq_step_p[16];
extern const u8 rkvenc_hevc_aq_thd[16];
extern const s8 rkvenc_hevc_aq_step[16];
extern const u16 rkvenc_hevc_intra_thd[4];
extern const u8 rkvenc_hevc_intra_wgt32[7];
extern const u8 rkvenc_hevc_intra_wgt16[7];

#endif
