/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _HANTRO_AV1_H_
#define _HANTRO_AV1_H_

#define AV1_PRIMARY_REF_NONE	7
#define AV1_REF_SCALE_SHIFT	14
#define MAX_FRAME_DISTANCE	31
#define AV1DEC_MAX_PIC_BUFFERS	24

#define SCALE_NUMERATOR 8
#define SUPERRES_SCALE_DENOMINATOR_MIN (SCALE_NUMERATOR + 1)

#define RS_SCALE_SUBPEL_BITS 14
#define RS_SCALE_SUBPEL_MASK ((1 << RS_SCALE_SUBPEL_BITS) - 1)
#define RS_SUBPEL_BITS 6
#define RS_SUBPEL_MASK ((1 << RS_SUBPEL_BITS) - 1)
#define RS_SCALE_EXTRA_BITS (RS_SCALE_SUBPEL_BITS - RS_SUBPEL_BITS)
#define RS_SCALE_EXTRA_OFF (1 << (RS_SCALE_EXTRA_BITS - 1))

/*
 * These 3 values aren't defined enum v4l2_av1_segment_feature because
 * they are not part of the specification
 */
#define V4L2_AV1_SEG_LVL_ALT_LF_Y_H	2
#define V4L2_AV1_SEG_LVL_ALT_LF_U	3
#define V4L2_AV1_SEG_LVL_ALT_LF_V	4

#define IS_INTRA(type) ((type == V4L2_AV1_KEY_FRAME) || (type == V4L2_AV1_INTRA_ONLY_FRAME))

#define LST_BUF_IDX (V4L2_AV1_REF_LAST_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define LST2_BUF_IDX (V4L2_AV1_REF_LAST2_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define LST3_BUF_IDX (V4L2_AV1_REF_LAST3_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define GLD_BUF_IDX (V4L2_AV1_REF_GOLDEN_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define BWD_BUF_IDX (V4L2_AV1_REF_BWDREF_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define ALT2_BUF_IDX (V4L2_AV1_REF_ALTREF2_FRAME - V4L2_AV1_REF_LAST_FRAME)
#define ALT_BUF_IDX (V4L2_AV1_REF_ALTREF_FRAME - V4L2_AV1_REF_LAST_FRAME)

int hantro_av1_get_frame_index(struct hantro_ctx *ctx, int ref);
int hantro_av1_get_order_hint(struct hantro_ctx *ctx, int ref);
int hantro_av1_frame_ref(struct hantro_ctx *ctx, u64 timestamp);
void hantro_av1_clean_refs(struct hantro_ctx *ctx);
size_t hantro_av1_luma_size(struct hantro_ctx *ctx);
size_t hantro_av1_chroma_size(struct hantro_ctx *ctx);
void hantro_av1_exit(struct hantro_ctx *ctx);
int hantro_av1_init(struct hantro_ctx *ctx);
int hantro_av1_prepare_run(struct hantro_ctx *ctx);
void hantro_av1_set_global_model(struct hantro_ctx *ctx);
int hantro_av1_tile_log2(int target);
int hantro_av1_get_dist(struct hantro_ctx *ctx, int a, int b);
void hantro_av1_set_frame_sign_bias(struct hantro_ctx *ctx);
void hantro_av1_init_scaling_function(const u8 *values, const u8 *scaling,
				      u8 num_points, u8 *scaling_lut);
void hantro_av1_set_tile_info(struct hantro_ctx *ctx);
bool hantro_av1_is_lossless(struct hantro_ctx *ctx);
void hantro_av1_update_prob(struct hantro_ctx *ctx);
void hantro_av1_set_prob(struct hantro_ctx *ctx);

int hantro_av1_get_hardware_mcomp_filt_type(int interpolation_filter);
int hantro_av1_get_hardware_tx_mode(enum v4l2_av1_tx_mode tx_mode);

#endif
