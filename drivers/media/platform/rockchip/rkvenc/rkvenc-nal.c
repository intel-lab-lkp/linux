// SPDX-License-Identifier: GPL-2.0-only
/*
 * H.264 SPS/PPS (ITU-T H.264 7.3.2) and HEVC VPS/SPS/PPS (ITU-T H.265 7.3.2)
 * writer for the RKVENC v1 encoder.
 */

#include "rkvenc-nal.h"

#define NAL_RBSP_MAX		256

#define H264_NAL_SPS		7
#define H264_NAL_PPS		8
#define H264_PROFILE_HIGH	100
#define HEVC_NAL_VPS		32
#define HEVC_NAL_SPS		33
#define HEVC_NAL_PPS		34

struct nal_bw {
	u8 buf[NAL_RBSP_MAX];
	u32 bits;
	bool overflow;
};

static void bw_put(struct nal_bw *bw, u32 val, u32 bits)
{
	u32 i, bit;

	for (i = bits; i > 0; i--) {
		if (bw->bits >= 8 * NAL_RBSP_MAX) {
			bw->overflow = true;
			return;
		}
		bit = (val >> (i - 1)) & 1;
		if (bit)
			bw->buf[bw->bits / 8] |= 0x80 >> (bw->bits % 8);
		bw->bits++;
	}
}

static void bw_ue(struct nal_bw *bw, u32 val)
{
	u32 len = 32 - __builtin_clz(val + 1);

	bw_put(bw, 0, len - 1);
	bw_put(bw, val + 1, len);
}

static void bw_se(struct nal_bw *bw, s32 val)
{
	bw_ue(bw, val > 0 ? 2 * val - 1 : -2 * val);
}

static void bw_trailing(struct nal_bw *bw)
{
	bw_put(bw, 1, 1);
	while (bw->bits % 8)
		bw_put(bw, 0, 1);
}

static int nal_emit(const struct nal_bw *bw, const u8 *hdr, u32 hdr_len,
		    u8 *out, u32 size)
{
	u32 len = bw->bits / 8, pos = 0, zeros = 0, i;

	if (bw->overflow || size < 4 + hdr_len)
		return -ENOSPC;

	out[pos++] = 0;
	out[pos++] = 0;
	out[pos++] = 0;
	out[pos++] = 1;
	for (i = 0; i < hdr_len; i++)
		out[pos++] = hdr[i];

	/* Emulation prevention, H.264 7.4.1 */
	for (i = 0; i < len; i++) {
		if (pos + 2 > size)
			return -ENOSPC;
		if (zeros == 2 && bw->buf[i] <= 3) {
			out[pos++] = 3;
			zeros = 0;
		}
		out[pos++] = bw->buf[i];
		zeros = bw->buf[i] ? 0 : zeros + 1;
	}

	return pos;
}

static void h264_vui_write(struct nal_bw *bw, const struct rkvenc_h264_seq *seq)
{
	bw_put(bw, 0, 4);
	bw_put(bw, seq->timing, 1);
	if (seq->timing) {
		bw_put(bw, 1, 32);
		bw_put(bw, 2 * seq->fps, 32);
		bw_put(bw, 1, 1);
	}
	bw_put(bw, 0, 3);
	bw_put(bw, 1, 1);	/* bitstream_restriction_flag */
	bw_put(bw, 1, 1);
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_ue(bw, 16);
	bw_ue(bw, 16);
	bw_ue(bw, 0);		/* max_num_reorder_frames */
	bw_ue(bw, 1);		/* max_dec_frame_buffering */
}

static void h264_sps_write(struct nal_bw *bw, const struct rkvenc_h264_seq *seq)
{
	u32 w16 = ALIGN(seq->width, 16), h16 = ALIGN(seq->height, 16);
	bool crop = w16 != seq->width || h16 != seq->height;
	u32 constraints;

	if (seq->profile_idc == H264_PROFILE_HIGH)
		constraints = 0;
	else if (seq->profile_idc == 66)
		constraints = 0xc0;
	else
		constraints = 0x40;

	bw_put(bw, seq->profile_idc, 8);
	bw_put(bw, constraints, 8);
	bw_put(bw, seq->level_idc, 8);
	bw_ue(bw, 0);
	if (seq->profile_idc == H264_PROFILE_HIGH) {
		bw_ue(bw, 1);
		bw_ue(bw, 0);
		bw_ue(bw, 0);
		bw_put(bw, 0, 2);
	}
	bw_ue(bw, seq->log2_max_frame_num_m4);
	bw_ue(bw, 0);
	bw_ue(bw, seq->log2_max_poc_lsb_m4);
	bw_ue(bw, 1);
	bw_put(bw, 0, 1);
	bw_ue(bw, w16 / 16 - 1);
	bw_ue(bw, h16 / 16 - 1);
	bw_put(bw, 1, 1);
	bw_put(bw, 1, 1);
	bw_put(bw, crop, 1);
	if (crop) {
		bw_ue(bw, 0);
		bw_ue(bw, (w16 - seq->width) / 2);
		bw_ue(bw, 0);
		bw_ue(bw, (h16 - seq->height) / 2);
	}
	bw_put(bw, 1, 1);
	h264_vui_write(bw, seq);
	bw_trailing(bw);
}

static void h264_pps_write(struct nal_bw *bw, const struct rkvenc_h264_seq *seq)
{
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_put(bw, seq->cabac, 1);
	bw_put(bw, 0, 1);
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_put(bw, 0, 3);
	bw_se(bw, seq->pic_init_qp - 26);
	bw_se(bw, 0);
	bw_se(bw, seq->chroma_qp_offset);
	bw_put(bw, 1, 1);
	bw_put(bw, 0, 2);
	if (seq->transform_8x8) {
		bw_put(bw, 1, 1);
		bw_put(bw, 0, 1);
		bw_se(bw, seq->chroma_qp_offset);
	}
	bw_trailing(bw);
}

int rkvenc_h264_headers_write(const struct rkvenc_h264_seq *seq, u8 *buf,
			      u32 size)
{
	u8 hdr;
	struct nal_bw bw;
	int len, pos;

	memset(&bw, 0, sizeof(bw));
	h264_sps_write(&bw, seq);
	hdr = 3 << 5 | H264_NAL_SPS;
	len = nal_emit(&bw, &hdr, 1, buf, size);
	if (len < 0)
		return len;
	pos = len;

	memset(&bw, 0, sizeof(bw));
	h264_pps_write(&bw, seq);
	hdr = 3 << 5 | H264_NAL_PPS;
	len = nal_emit(&bw, &hdr, 1, buf + pos, size - pos);
	if (len < 0)
		return len;

	return pos + len;
}

static void hevc_ptl_write(struct nal_bw *bw, const struct rkvenc_hevc_seq *seq)
{
	bw_put(bw, 0, 2);
	bw_put(bw, 0, 1);
	bw_put(bw, 1, 5);		/* general_profile_idc: Main */
	bw_put(bw, 0x60000000, 32);	/* compatible with profiles 1, 2 */
	bw_put(bw, 1, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 1, 1);
	bw_put(bw, 0, 32);
	bw_put(bw, 0, 12);
	bw_put(bw, seq->level_idc, 8);
}

static void hevc_vps_write(struct nal_bw *bw, const struct rkvenc_hevc_seq *seq)
{
	bw_put(bw, 0, 4);
	bw_put(bw, 1, 1);
	bw_put(bw, 1, 1);
	bw_put(bw, 0, 6);
	bw_put(bw, 0, 3);
	bw_put(bw, 1, 1);
	bw_put(bw, 0xffff, 16);
	hevc_ptl_write(bw, seq);
	bw_put(bw, 1, 1);
	bw_ue(bw, 1);
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_put(bw, 0, 6);
	bw_ue(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_trailing(bw);
}

static void hevc_sps_write(struct nal_bw *bw, const struct rkvenc_hevc_seq *seq)
{
	u32 w8 = ALIGN(seq->width, 8), h8 = ALIGN(seq->height, 8);
	bool crop = w8 != seq->width || h8 != seq->height;

	bw_put(bw, 0, 4);
	bw_put(bw, 0, 3);
	bw_put(bw, 1, 1);
	hevc_ptl_write(bw, seq);
	bw_ue(bw, 0);
	bw_ue(bw, 1);			/* chroma_format_idc 4:2:0 */
	bw_ue(bw, w8);
	bw_ue(bw, h8);
	bw_put(bw, crop, 1);
	if (crop) {
		bw_ue(bw, 0);
		bw_ue(bw, (w8 - seq->width) / 2);
		bw_ue(bw, 0);
		bw_ue(bw, (h8 - seq->height) / 2);
	}
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_ue(bw, seq->log2_max_poc_lsb_m4);
	bw_put(bw, 1, 1);
	bw_ue(bw, 1);			/* sps_max_dec_pic_buffering_minus1 */
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_ue(bw, 0);			/* min CU 8x8 */
	bw_ue(bw, 3);			/* CTU 64x64 */
	bw_ue(bw, 0);			/* min TU 4x4 */
	bw_ue(bw, 3);			/* max TU 32x32 */
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, seq->sao, 1);
	bw_put(bw, 0, 1);
	bw_ue(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, seq->strong_intra_smoothing, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_trailing(bw);
}

static void hevc_pps_write(struct nal_bw *bw, const struct rkvenc_hevc_seq *seq)
{
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 3);
	bw_put(bw, 0, 1);
	bw_put(bw, seq->cabac_init_present, 1);
	bw_ue(bw, 0);
	bw_ue(bw, 0);
	bw_se(bw, seq->pic_init_qp - 26);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 1, 1);		/* cu_qp_delta_enabled_flag */
	bw_ue(bw, 0);
	bw_se(bw, 0);
	bw_se(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, 1, 1);
	bw_put(bw, 0, 1);
	bw_put(bw, seq->deblock_disabled, 1);
	if (!seq->deblock_disabled) {
		bw_se(bw, seq->beta_offset);
		bw_se(bw, seq->tc_offset);
	}
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_ue(bw, 0);
	bw_put(bw, 0, 1);
	bw_put(bw, 0, 1);
	bw_trailing(bw);
}

int rkvenc_hevc_headers_write(const struct rkvenc_hevc_seq *seq, u8 *buf,
			      u32 size)
{
	static const u8 types[] = { HEVC_NAL_VPS, HEVC_NAL_SPS, HEVC_NAL_PPS };
	struct nal_bw bw;
	int len, pos;
	u8 hdr[2];
	u32 i;

	pos = 0;
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		memset(&bw, 0, sizeof(bw));
		if (types[i] == HEVC_NAL_VPS)
			hevc_vps_write(&bw, seq);
		else if (types[i] == HEVC_NAL_SPS)
			hevc_sps_write(&bw, seq);
		else
			hevc_pps_write(&bw, seq);
		hdr[0] = types[i] << 1;
		hdr[1] = 1;
		len = nal_emit(&bw, hdr, 2, buf + pos, size - pos);
		if (len < 0)
			return len;
		pos += len;
	}

	return pos;
}
