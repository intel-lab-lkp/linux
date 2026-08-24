// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - Resizer
 *
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 */

#include "rkisp2-common.h"

/*
 * Although there is no resizer subdevice like rkisp1, there is significant
 * control handling that this is split into a separate file. This includes
 * handling of both the dual crop and resizer.
 *
 * In rkisp2, crop rectangles are set by parameter buffers and are thus handled
 * by params.
 */

struct rkisp2_resizer_config {
	int max_rsz_width;
	int max_rsz_height;
	int min_rsz_width;
	int min_rsz_height;
	struct {
		u32 ctrl;
		u32 ctrl_shd;
		u32 update;
		u32 src_size;
		u32 dst_size;
		u32 scale_hy_offs_mi;
		u32 scale_hc_offs_mi;
		u32 scale_in_crop_offs;
		u32 scale_hy_offs;
		u32 scale_hc_offs;
		u32 scale_hy_size;
		u32 scale_hc_size;
		u32 scale_hy;
		u32 scale_hcr;
		u32 scale_hcb;
		u32 scale_vy;
		u32 scale_vc;
		u32 scale_lut;
		u32 scale_lut_addr;
		u32 scale_hy_shd;
		u32 scale_hcr_shd;
		u32 scale_hcb_shd;
		u32 scale_vy_shd;
		u32 scale_vc_shd;
		u32 phase_hy;
		u32 phase_hc;
		u32 phase_vy;
		u32 phase_vc;
		u32 phase_hy_shd;
		u32 phase_hc_shd;
		u32 phase_vy_shd;
		u32 phase_vc_shd;
	} rsz;
	struct {
		u32 ctrl;
		u32 yuvmode_mask;
		u32 rawmode_mask;
		u32 h_offset;
		u32 v_offset;
		u32 h_size;
		u32 v_size;
	} dual_crop;
};

static const struct rkisp2_resizer_config rkisp2_resizer_config_mp = {
	.rsz = {
		.ctrl = RKISP2_CIF_MRSZ_CTRL,
		.scale_hy = RKISP2_CIF_MRSZ_SCALE_HY,
		.scale_hcr = RKISP2_CIF_MRSZ_SCALE_HCR,
		.scale_hcb = RKISP2_CIF_MRSZ_SCALE_HCB,
		.scale_vy = RKISP2_CIF_MRSZ_SCALE_VY,
		.scale_vc = RKISP2_CIF_MRSZ_SCALE_VC,
		.scale_lut = RKISP2_CIF_MRSZ_SCALE_LUT,
		.scale_lut_addr = RKISP2_CIF_MRSZ_SCALE_LUT_ADDR,
		.scale_hy_shd = RKISP2_CIF_MRSZ_SCALE_HY_SHD,
		.scale_hcr_shd = RKISP2_CIF_MRSZ_SCALE_HCR_SHD,
		.scale_hcb_shd = RKISP2_CIF_MRSZ_SCALE_HCB_SHD,
		.scale_vy_shd = RKISP2_CIF_MRSZ_SCALE_VY_SHD,
		.scale_vc_shd = RKISP2_CIF_MRSZ_SCALE_VC_SHD,
		.phase_hy = RKISP2_CIF_MRSZ_PHASE_HY,
		.phase_hc = RKISP2_CIF_MRSZ_PHASE_HC,
		.phase_vy = RKISP2_CIF_MRSZ_PHASE_VY,
		.phase_vc = RKISP2_CIF_MRSZ_PHASE_VC,
		.ctrl_shd = RKISP2_CIF_MRSZ_CTRL_SHD,
		.phase_hy_shd = RKISP2_CIF_MRSZ_PHASE_HY_SHD,
		.phase_hc_shd = RKISP2_CIF_MRSZ_PHASE_HC_SHD,
		.phase_vy_shd = RKISP2_CIF_MRSZ_PHASE_VY_SHD,
		.phase_vc_shd = RKISP2_CIF_MRSZ_PHASE_VC_SHD,
	},
	.dual_crop = {
		.ctrl = RKISP2_CIF_DUAL_CROP_CTRL,
		.yuvmode_mask = RKISP2_CIF_DUAL_CROP_MP_MODE_YUV,
		.rawmode_mask = RKISP2_CIF_DUAL_CROP_MP_MODE_RAW,
		.h_offset = RKISP2_CIF_DUAL_CROP_M_H_OFFS,
		.v_offset = RKISP2_CIF_DUAL_CROP_M_V_OFFS,
		.h_size = RKISP2_CIF_DUAL_CROP_M_H_SIZE,
		.v_size = RKISP2_CIF_DUAL_CROP_M_V_SIZE,
	},
};

static const struct rkisp2_resizer_config rkisp2_resizer_config_sp = {
	.rsz = {
		.ctrl = RKISP2_CIF_SRSZ_CTRL,
		.scale_hy = RKISP2_CIF_SRSZ_SCALE_HY,
		.scale_hcr = RKISP2_CIF_SRSZ_SCALE_HCR,
		.scale_hcb = RKISP2_CIF_SRSZ_SCALE_HCB,
		.scale_vy = RKISP2_CIF_SRSZ_SCALE_VY,
		.scale_vc = RKISP2_CIF_SRSZ_SCALE_VC,
		.scale_lut = RKISP2_CIF_SRSZ_SCALE_LUT,
		.scale_lut_addr = RKISP2_CIF_SRSZ_SCALE_LUT_ADDR,
		.scale_hy_shd = RKISP2_CIF_SRSZ_SCALE_HY_SHD,
		.scale_hcr_shd = RKISP2_CIF_SRSZ_SCALE_HCR_SHD,
		.scale_hcb_shd = RKISP2_CIF_SRSZ_SCALE_HCB_SHD,
		.scale_vy_shd = RKISP2_CIF_SRSZ_SCALE_VY_SHD,
		.scale_vc_shd = RKISP2_CIF_SRSZ_SCALE_VC_SHD,
		.phase_hy = RKISP2_CIF_SRSZ_PHASE_HY,
		.phase_hc = RKISP2_CIF_SRSZ_PHASE_HC,
		.phase_vy = RKISP2_CIF_SRSZ_PHASE_VY,
		.phase_vc = RKISP2_CIF_SRSZ_PHASE_VC,
		.ctrl_shd = RKISP2_CIF_SRSZ_CTRL_SHD,
		.phase_hy_shd = RKISP2_CIF_SRSZ_PHASE_HY_SHD,
		.phase_hc_shd = RKISP2_CIF_SRSZ_PHASE_HC_SHD,
		.phase_vy_shd = RKISP2_CIF_SRSZ_PHASE_VY_SHD,
		.phase_vc_shd = RKISP2_CIF_SRSZ_PHASE_VC_SHD,
	},
	.dual_crop = {
		.ctrl = RKISP2_CIF_DUAL_CROP_CTRL,
		.yuvmode_mask = RKISP2_CIF_DUAL_CROP_SP_MODE_YUV,
		.rawmode_mask = RKISP2_CIF_DUAL_CROP_SP_MODE_RAW,
		.h_offset = RKISP2_CIF_DUAL_CROP_S_H_OFFS,
		.v_offset = RKISP2_CIF_DUAL_CROP_S_V_OFFS,
		.h_size = RKISP2_CIF_DUAL_CROP_S_H_SIZE,
		.v_size = RKISP2_CIF_DUAL_CROP_S_V_SIZE,
	},
};

static bool rkisp2_rect_equals(const struct v4l2_rect *a,
			       const struct v4l2_rect *b)
{
	return a->left == b->left && a->top == b->top &&
	       a->width == b->width && a->height == b->height;
}

static void rkisp2_resizer_disable_dcrop(struct rkisp2_resizer *rsz)
{
	struct rkisp2_device *rkisp2 = rsz->rkisp2;
	const struct rkisp2_resizer_config *config = rsz->config;

	u32 mask = config->dual_crop.yuvmode_mask |
		   config->dual_crop.rawmode_mask;
	u32 ctrl = rkisp2_read(rkisp2, config->dual_crop.ctrl);

	ctrl &= ~mask;
	ctrl |= RKISP2_CIF_DUAL_CROP_CFG_UPD;
	rkisp2_write(rkisp2, config->dual_crop.ctrl, ctrl);
}

static void rkisp2_resizer_config_dcrop(struct rkisp2_resizer *rsz,
					bool is_raw,
					const struct v4l2_rect *crop)
{
	struct rkisp2_device *rkisp2 = rsz->rkisp2;
	const struct rkisp2_resizer_config *config = rsz->config;

	u32 mask = config->dual_crop.yuvmode_mask |
		   config->dual_crop.rawmode_mask;
	u32 ctrl = rkisp2_read(rkisp2, config->dual_crop.ctrl);

	rkisp2_write(rkisp2, config->dual_crop.h_offset, crop->left);
	rkisp2_write(rkisp2, config->dual_crop.v_offset, crop->top);
	rkisp2_write(rkisp2, config->dual_crop.h_size, crop->width);
	rkisp2_write(rkisp2, config->dual_crop.v_size, crop->height);

	ctrl &= ~mask;
	if (is_raw)
		ctrl |= config->dual_crop.rawmode_mask;
	else
		ctrl |= config->dual_crop.yuvmode_mask;
	ctrl |= RKISP2_CIF_DUAL_CROP_CFG_UPD;
	rkisp2_write(rkisp2, config->dual_crop.ctrl, ctrl);
}

static void rkisp2_resizer_disable_rsz(struct rkisp2_resizer *rsz)
{
	struct rkisp2_device *rkisp2 = rsz->rkisp2;
	const struct rkisp2_resizer_config *config = rsz->config;

	rkisp2_write(rkisp2, config->rsz.ctrl, 0);
	rkisp2_write(rkisp2, config->rsz.ctrl, RKISP2_CIF_RSZ_CTRL_CFG_UPD);
}

static int rkisp2_mbus_code_xysubs(u32 code, u32 *xsubs, u32 *ysubs)
{
	switch (code) {
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_YVYU8_1X16:
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_VYUY8_1X16:
		*xsubs = 2;
		*ysubs = 1;
		return 0;
	default:
		*xsubs = 1;
		*ysubs = 1;
		return 0;
	}
}

static void rkisp2_format_xysubs(const struct v4l2_format_info *info,
				 u32 *xsubs, u32 *ysubs)
{
	if (info && v4l2_is_format_yuv(info)) {
		*xsubs = info->hdiv ?: 1;
		*ysubs = info->vdiv ?: 1;
	} else {
		*xsubs = 1;
		*ysubs = 1;
	}
}

static u32 rkisp2_scale_up_factor(u32 src, u32 dst)
{
	if (src <= 1 || dst <= 1)
		return RKISP2_CIF_RSZ_SCALER_FACTOR;
	return ((src - 1) * RKISP2_CIF_RSZ_SCALER_FACTOR) / (dst - 1);
}

static u32 rkisp2_scale_down_factor(u32 src, u32 dst)
{
	if (src <= 1 || dst <= 1)
		return RKISP2_CIF_RSZ_SCALER_FACTOR;
	return ((dst - 1) * RKISP2_CIF_RSZ_SCALER_FACTOR) / (src - 1) + 1;
}

static void rkisp2_resizer_prepare_lut(struct rkisp2_resizer *rsz)
{
	struct rkisp2_device *rkisp2 = rsz->rkisp2;
	const struct rkisp2_resizer_config *config = rsz->config;
	unsigned int i;

	for (i = 0; i < 64; i++) {
		rkisp2_write(rkisp2, config->rsz.scale_lut_addr, i);
		rkisp2_write(rkisp2, config->rsz.scale_lut, i);
	}
}

static void rkisp2_resizer_config_rsz(struct rkisp2_resizer *rsz,
				      const struct v4l2_rect *in_rect,
				      const struct v4l2_rect *out_rect,
				      const struct v4l2_mbus_framefmt *src_fmt)
{
	struct rkisp2_device *rkisp2 = rsz->rkisp2;
	const struct rkisp2_resizer_config *config = rsz->config;
	struct v4l2_rect in_c, out_c;
	u32 in_xsubs = 1, in_ysubs = 1;
	u32 out_xsubs = 1, out_ysubs = 1;
	u32 rsz_ctrl = 0;

	rkisp2_mbus_code_xysubs(src_fmt->code, &in_xsubs, &in_ysubs);
	rkisp2_format_xysubs(rsz->source_fmt_info, &out_xsubs, &out_ysubs);

	in_c.width = DIV_ROUND_UP(in_rect->width, in_xsubs);
	in_c.height = DIV_ROUND_UP(in_rect->height, in_ysubs);
	out_c.width = DIV_ROUND_UP(out_rect->width, out_xsubs);
	out_c.height = DIV_ROUND_UP(out_rect->height, out_ysubs);

	rkisp2_write(rkisp2, config->rsz.phase_hy, 0);
	rkisp2_write(rkisp2, config->rsz.phase_hc, 0);
	rkisp2_write(rkisp2, config->rsz.phase_vy, 0);
	rkisp2_write(rkisp2, config->rsz.phase_vc, 0);

	rkisp2_resizer_prepare_lut(rsz);

	if (in_rect->width != out_rect->width) {
		u32 hy;

		rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_HY_ENABLE;
		if (in_rect->width < out_rect->width) {
			hy = rkisp2_scale_up_factor(in_rect->width, out_rect->width);
			rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_HY_UP;
		} else {
			hy = rkisp2_scale_down_factor(in_rect->width, out_rect->width);
		}
		rkisp2_write(rkisp2, config->rsz.scale_hy, hy);
	}

	if (in_c.width != out_c.width) {
		u32 hc;

		rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_HC_ENABLE;
		if (in_c.width < out_c.width) {
			hc = rkisp2_scale_up_factor(in_c.width, out_c.width);
			rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_HC_UP;
		} else {
			hc = rkisp2_scale_down_factor(in_c.width, out_c.width);
		}
		rkisp2_write(rkisp2, config->rsz.scale_hcb, hc);
		rkisp2_write(rkisp2, config->rsz.scale_hcr, hc);
	}

	if (in_rect->height != out_rect->height) {
		u32 vy;

		rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_VY_ENABLE;
		if (in_rect->height < out_rect->height) {
			vy = rkisp2_scale_up_factor(in_rect->height, out_rect->height);
			rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_VY_UP;
		} else {
			vy = rkisp2_scale_down_factor(in_rect->height, out_rect->height);
		}
		rkisp2_write(rkisp2, config->rsz.scale_vy, vy);
	}

	if (in_c.height != out_c.height) {
		u32 vc;

		rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_VC_ENABLE;
		if (in_c.height < out_c.height) {
			vc = rkisp2_scale_up_factor(in_c.height, out_c.height);
			rsz_ctrl |= RKISP2_CIF_RSZ_CTRL_SCALE_VC_UP;
		} else {
			vc = rkisp2_scale_down_factor(in_c.height, out_c.height);
		}
		rkisp2_write(rkisp2, config->rsz.scale_vc, vc);
	}

	rkisp2_write(rkisp2, config->rsz.ctrl,
		rsz_ctrl | RKISP2_CIF_RSZ_CTRL_CFG_UPD);
}

void rkisp2_resizer_configure(struct rkisp2_resizer *rsz,
			      struct v4l2_rect *crop)
{
	bool is_raw = v4l2_is_format_bayer(rsz->source_fmt_info);

	dev_dbg(rsz->rkisp2->dev,
		"%s: Configuring crop for path %d from sink size (%d,%d)/%dx%d, crop (%d,%d)/%dx%d, source size (%d,%d)/%dx%d\n",
		__func__, rsz->id,
		rsz->sink_size.left, rsz->sink_size.top, rsz->sink_size.width, rsz->sink_size.height,
		crop->left, crop->top, crop->width, crop->height,
		rsz->source_size.left, rsz->source_size.top, rsz->source_size.width, rsz->source_size.height);

	/* Apparently dual crop works on raw (untested) but resizer does not */
	if (rkisp2_has_feature(rsz->rkisp2, DUAL_CROP)) {
		if (rkisp2_rect_equals(crop, &rsz->sink_size))
			rkisp2_resizer_disable_dcrop(rsz);
		else
			rkisp2_resizer_config_dcrop(rsz, is_raw, crop);
	}

	if (crop->width == rsz->source_size.width && crop->height == rsz->source_size.height)
		rkisp2_resizer_disable_rsz(rsz);
	else
		rkisp2_resizer_config_rsz(rsz, crop,
					  &rsz->source_size,
					  &rsz->source_format);
}

void rkisp2_resizer_pre_configure(struct rkisp2_device *rkisp2,
				  const struct v4l2_mbus_framefmt *sink_frm,
				  const struct v4l2_mbus_framefmt *mp_src_frm,
				  const struct v4l2_mbus_framefmt *sp_src_frm)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		struct rkisp2_resizer *rsz = &rkisp2->resizer_devs[i];
		struct rkisp2_capture *cap = &rkisp2->capture_devs[i];
		struct v4l2_pix_format_mplane *cap_fmt = &cap->pix.fmt;

		rsz->sink_size.left = 0;
		rsz->sink_size.top = 0;
		rsz->sink_size.width = sink_frm->width;
		rsz->sink_size.height = sink_frm->height;

		rsz->source_size.left = 0;
		rsz->source_size.top = 0;
		rsz->source_size.width = cap_fmt->width;
		rsz->source_size.height = cap_fmt->height;

		rsz->source_format = (i == 0 ? *mp_src_frm : *sp_src_frm);
		rsz->source_fmt_info = cap->pix.info;
	}
}

void rkisp2_resizer_devs_init(struct rkisp2_device *rkisp2)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		struct rkisp2_resizer *rsz = &rkisp2->resizer_devs[i];
		rsz->rkisp2 = rkisp2;
		rsz->id = i;
		rsz->config = (i == RKISP2_MAINPATH ?
			       &rkisp2_resizer_config_mp :
			       &rkisp2_resizer_config_sp);
	}
}
