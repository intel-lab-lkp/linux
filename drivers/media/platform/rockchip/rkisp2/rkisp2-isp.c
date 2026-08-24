// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - ISP Subdevice
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>

#include <media/v4l2-event.h>

#include "rkisp2-common.h"

#define RKISP2_DEF_SINK_PAD_FMT MEDIA_BUS_FMT_SRGGB10_1X10
#define RKISP2_DEF_SRC_PAD_FMT MEDIA_BUS_FMT_YUYV8_2X8

#define RKISP2_ISP_DEV_NAME	RKISP2_DRIVER_NAME "_isp"

static u16 rkisp2_isp_get_active_sink_pad(struct rkisp2_isp *isp)
{
	struct media_entity *entity = &isp->sd.entity;
	struct media_link *link;

	list_for_each_entry(link, &entity->links, list) {
		if (link->sink->entity != entity ||
		    (link->sink->index != RKISP2_ISP_PAD_SINK_VIDEO_DMA_0 &&
		     link->sink->index != RKISP2_ISP_PAD_SINK_VIDEO_DMA_1 &&
		     link->sink->index != RKISP2_ISP_PAD_SINK_VIDEO_DMA_2))
			continue;

		if (link->flags & MEDIA_LNK_FL_ENABLED) {
			dev_dbg(isp->rkisp2->dev, "%s: active link is %d\n",
				__func__, link->sink->index);
			return link->sink->index;
		}
	}

	/* Default to 0th DMA if no link is active */
	return RKISP2_ISP_PAD_SINK_VIDEO_DMA_0;
}

/* ----------------------------------------------------------------------------
 * Camera Interface registers configurations
 */

/* Image stabilization */
static void rkisp2_config_ism(struct rkisp2_isp *isp,
		const struct v4l2_subdev_state *sd_state)
{
	const struct v4l2_mbus_framefmt *sink_frm =
		v4l2_subdev_state_get_format(sd_state,
					     rkisp2_isp_get_active_sink_pad(isp));
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	u32 val;

	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_RECENTER, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_MAX_DX, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_MAX_DY, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_DISPLACE, 0);

	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_H_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_V_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_H_SIZE, sink_frm->width);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_V_SIZE, sink_frm->height);

	/*
	 * IS (Image Stabilization) is always on, working as output crop
	 * - On rk3588 there is no image stabilizer, but the crop portion of it
	 *   exists and must be configured for the pipeline to function
	 */
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IS_CTRL, 1);
	val = rkisp2_read(rkisp2, RKISP2_CIF_ISP_CTRL);
	val |= RKISP2_CIF_ISP_CTRL_ISP_CFG_UPD;
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_CTRL, val);
}

/*
 * configure ISP blocks with input format, size......
 */
static int rkisp2_config_isp(struct rkisp2_isp *isp,
			     const struct v4l2_subdev_state *sd_state)
{
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	u32 isp_ctrl = 0, irq_mask = 0, acq_mult = 0, acq_prop = 0;
	const struct rkisp2_mbus_info *sink_fmt;
	const struct rkisp2_mbus_info *mp_src_fmt;
	const struct v4l2_mbus_framefmt *mp_src_frm;
	const struct v4l2_mbus_framefmt *sp_src_frm;
	const struct v4l2_mbus_framefmt *sink_frm;

	/* All the video sink formats and crops are kept equal */
	sink_frm = v4l2_subdev_state_get_format(sd_state,
						RKISP2_ISP_PAD_SINK_VIDEO_DMA_0);
	/*
	 * This is only used to check if we are doing raw -> raw or raw -> yuv
	 * or yuv -> yuv so we only need to check the main path source pad
	 * format
	 */
	mp_src_frm = v4l2_subdev_state_get_format(sd_state,
					       RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN);

	sink_fmt = rkisp2_mbus_info_get_by_code(sink_frm->code);
	mp_src_fmt = rkisp2_mbus_info_get_by_code(mp_src_frm->code);

	if (sink_fmt->pixel_enc == V4L2_PIXEL_ENC_BAYER) {
		acq_mult = 1;
		if (mp_src_fmt->pixel_enc == V4L2_PIXEL_ENC_BAYER) {
			isp_ctrl = RKISP2_CIF_ISP_CTRL_ISP_MODE_RAW_PICT;
		} else {
			rkisp2_write(rkisp2, ISP_DEBAYER_CONTROL,
				     SW_DEBAYER_EN | SW_DEBAYER_FILTER_G_EN |
				     SW_DEBAYER_FILTER_C_EN);

			isp_ctrl = RKISP2_CIF_ISP_CTRL_ISP_MODE_BAYER_ITU601;
		}
	} else if (sink_fmt->pixel_enc == V4L2_PIXEL_ENC_YUV) {
		acq_mult = 2;
		isp_ctrl = RKISP2_CIF_ISP_CTRL_ISP_MODE_ITU601;
		irq_mask |= RKISP2_CIF_ISP_DATA_LOSS;
	}

	rkisp2_write(rkisp2, RKISP2_CIF_ISP_CTRL, isp_ctrl);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_PROP,
		     acq_prop | sink_fmt->yuv_seq |
		     RKISP2_CIF_ISP_ACQ_PROP_BAYER_PAT(sink_fmt->bayer_pat) |
		     RKISP2_CIF_ISP_ACQ_PROP_FIELD_SEL_ALL);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_NR_FRAMES, 0);

	/* Acquisition Size */
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_H_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_V_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_H_SIZE,
		     acq_mult * sink_frm->width);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ACQ_V_SIZE, sink_frm->height);

	/* ISP Out Area */
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_OUT_H_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_OUT_V_OFFS, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_OUT_H_SIZE, sink_frm->width);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_OUT_V_SIZE, sink_frm->height);

	irq_mask |= RKISP2_CIF_ISP_FRAME | RKISP2_CIF_ISP_V_START |
		    RKISP2_CIF_ISP_PIC_SIZE_ERROR;
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IMSC, irq_mask);

	sp_src_frm = v4l2_subdev_state_get_format(sd_state,
					       RKISP2_ISP_PAD_SOURCE_VIDEO_SELF);
	rkisp2_params_pre_configure(&rkisp2->params, sink_fmt->bayer_pat,
				    sink_frm, mp_src_frm, sp_src_frm);

	isp->sink_fmt = sink_fmt;

	return 0;
}

/* Configure MUX */
static void rkisp2_config_path(struct rkisp2_isp *isp)
{
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	u32 dpcl = rkisp2_read(rkisp2, RKISP2_CIF_VI_DPCL);

	dpcl |= RKISP2_CIF_VI_DPCL_IF_SEL_MIPI;
	rkisp2_write(rkisp2, RKISP2_CIF_VI_DPCL, dpcl);
}

/* Hardware configure Entry */
static int rkisp2_config_cif(struct rkisp2_isp *isp,
			     struct v4l2_subdev_state *sd_state)
{
	int ret;

	ret = rkisp2_config_isp(isp, sd_state);
	if (ret)
		return ret;

	rkisp2_config_ism(isp, sd_state);
	rkisp2_config_path(isp);

	return 0;
}

static void rkisp2_isp_stop(struct rkisp2_isp *isp)
{
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	u32 val;

	/*
	 * ISP(mi) stop in mi frame end -> Stop ISP(mipi) ->
	 * Stop ISP(isp) ->wait for ISP isp off
	 */

	/* Mask MIPI, MI, ISP, and STATS3A interrupts */
	rkisp2_write(rkisp2, RKISP2_CIF_MIPI_IMSC, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_IMSC, 0);
	rkisp2_write(rkisp2, RKISP2_CIF_MI_IMSC, 0);
	rkisp2_write(rkisp2, ISP_ISP3A_IMSC, 0);

	/* Flush posted writes */
	rkisp2_read(rkisp2, RKISP2_CIF_MIPI_IMSC);
	rkisp2_read(rkisp2, RKISP2_CIF_MI_IMSC);
	rkisp2_read(rkisp2, ISP_ISP3A_IMSC);

	/*
	 * Wait until the IRQ handlers have ended. The IRQ handler may still get
	 * invoked later (shared interrupt lines), but it will return immediately as
	 * the interrupts have now been masked.
	 */
	for (unsigned int il = 0; il < ARRAY_SIZE(rkisp2->irqs); ++il) {
		int irq = rkisp2->irqs[il];

		if (irq < 0)
			continue;
		if (il > 0 && rkisp2->irqs[il - 1] == irq)
			continue;

		synchronize_irq(irq);
	}

	/* Clear MIPI, MI, ISP, and STATS3A interrupt status */
	rkisp2_write(rkisp2, RKISP2_CIF_MIPI_ICR, ~0);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ICR, ~0);
	rkisp2_write(rkisp2, RKISP2_CIF_MI_ICR, ~0);
	rkisp2_write(rkisp2, ISP_ISP3A_ICR, ~0);

	/* stop ISP */
	val = rkisp2_read(rkisp2, RKISP2_CIF_ISP_CTRL);
	val &= ~(RKISP2_CIF_ISP_CTRL_ISP_INFORM_ENABLE |
		 RKISP2_CIF_ISP_CTRL_ISP_ENABLE);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_CTRL, val);

	val = rkisp2_read(rkisp2, RKISP2_CIF_ISP_CTRL);
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_CTRL,
		     val | RKISP2_CIF_ISP_CTRL_ISP_CFG_UPD);

	readx_poll_timeout(readl, rkisp2->base_addr + RKISP2_CIF_ISP_RIS,
			   val, val & RKISP2_CIF_ISP_OFF, 20, 100);
	/* No need to reset stats3a, as the ISP soft reset covers it */
	rkisp2_write(rkisp2, RKISP2_CIF_VI_IRCL,
		     RKISP2_CIF_VI_IRCL_MIPI_SW_RST |
		     RKISP2_CIF_VI_IRCL_ISP_SW_RST);
	rkisp2_write(rkisp2, RKISP2_CIF_VI_IRCL, 0x0);
}

static void rkisp2_config_clk(struct rkisp2_isp *isp)
{
	struct rkisp2_device *rkisp2 = isp->rkisp2;

	u32 val = (RKISP2_CIF_ICCL_ISP_CLK | RKISP2_CIF_ICCL_CP_CLK |
		   RKISP2_CIF_ICCL_MRSZ_CLK | RKISP2_CIF_ICCL_SRSZ_CLK |
		   RKISP2_CIF_ICCL_JPEG_CLK | RKISP2_CIF_ICCL_MI_CLK |
		   RKISP2_CIF_ICCL_IE_CLK | RKISP2_CIF_ICCL_MIPI_CLK |
		   RKISP2_CIF_ICCL_DCROP_CLK | ICCL_MPFBC_CLK);

	rkisp2_write(rkisp2, RKISP2_CIF_VI_ICCL, val);

	val = RKISP2_CIF_CLK_CTRL_ISP_3A | RKISP2_CIF_CLK_CTRL_ISP_RAW;
	rkisp2_write(rkisp2, RKISP2_CIF_VI_ISP_CLK_CTRL_V12, val);

	val = RKISP2_CIF_SWS_MIPI_DROP_FRM_DIS	| RKISP2_CIF_SWS_ACK_FRM_PRO_DIS;
	rkisp2_write(rkisp2, ISP3X_SWS_CFG, val);
}

static int rkisp2_isp_start(struct rkisp2_isp *isp,
			    const struct v4l2_subdev_state *sd_state,
			    struct media_pad *source)
{
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	u32 val;

	rkisp2_config_clk(isp);

	/* Activate ISP */
	val = rkisp2_read(rkisp2, RKISP2_CIF_ISP_CTRL);
	val |= RKISP2_CIF_ISP_CTRL_ISP_CFG_UPD |
	       RKISP2_CIF_ISP_CTRL_ISP_ENABLE |
	       RKISP2_CIF_ISP_CTRL_ISP_INFORM_ENABLE | RKISP2_CIF_ISP_CTRL_ISP_CFG_UPD_PERMANENT;
	rkisp2_write(rkisp2, RKISP2_CIF_ISP_CTRL, val);

	rkisp2_params_post_configure(&rkisp2->params);

	return 0;
}

/* ----------------------------------------------------------------------------
 * Subdev pad operations
 */

static inline struct rkisp2_isp *to_rkisp2_isp(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rkisp2_isp, sd);
}

static int rkisp2_isp_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	unsigned int i, dir;
	int pos = 0;

	if (code->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_0 ||
	    code->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_1 ||
	    code->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_2) {
		dir = RKISP2_ISP_SD_SINK;
	} else if (code->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN ||
		   code->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_SELF) {
		dir = RKISP2_ISP_SD_SRC;
	} else {
		if (code->index > 0)
			return -EINVAL;
		code->code = MEDIA_BUS_FMT_METADATA_FIXED;
		return 0;
	}

	for (i = 0; ; i++) {
		const struct rkisp2_mbus_info *fmt =
			rkisp2_mbus_info_get_by_index(i);

		if (!fmt)
			return -EINVAL;

		if (fmt->direction & dir)
			pos++;

		if (code->index == pos - 1) {
			code->code = fmt->mbus_code;
			if (fmt->pixel_enc == V4L2_PIXEL_ENC_YUV &&
			    dir == RKISP2_ISP_SD_SRC)
				code->flags =
					V4L2_SUBDEV_MBUS_CODE_CSC_QUANTIZATION;
			return 0;
		}
	}

	return -EINVAL;
}

static int rkisp2_isp_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *sd_state,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	struct rkisp2_isp *isp = to_rkisp2_isp(sd);
	const struct rkisp2_mbus_info *mbus_info;

	if (fse->index > 0)
		return -EINVAL;

	mbus_info = rkisp2_mbus_info_get_by_code(fse->code);
	if (!mbus_info)
		return -EINVAL;

	if (!(mbus_info->direction & RKISP2_ISP_SD_SINK) &&
	    (fse->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_0 ||
	     fse->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_1 ||
	     fse->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_2))
		return -EINVAL;

	if (!(mbus_info->direction & RKISP2_ISP_SD_SRC) &&
	    (fse->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN ||
	     fse->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_SELF))
		return -EINVAL;

	fse->min_width = RKISP2_ISP_MIN_WIDTH;
	fse->max_width = isp->rkisp2->info->max_width;
	fse->min_height = RKISP2_ISP_MIN_HEIGHT;
	fse->max_height = isp->rkisp2->info->max_height;

	return 0;
}

static int rkisp2_isp_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	enum rkisp2_isp_pad pad;

	for (pad = RKISP2_ISP_PAD_SINK_VIDEO_DMA_0;
	     pad < RKISP2_ISP_PAD_SINK_VIDEO_DMA_MAX; pad++) {
		sink_fmt = v4l2_subdev_state_get_format(sd_state, pad);
		sink_fmt->width = RKISP2_DEFAULT_WIDTH;
		sink_fmt->height = RKISP2_DEFAULT_HEIGHT;
		sink_fmt->field = V4L2_FIELD_NONE;
		sink_fmt->code = RKISP2_DEF_SINK_PAD_FMT;
		sink_fmt->colorspace = V4L2_COLORSPACE_RAW;
		sink_fmt->xfer_func = V4L2_XFER_FUNC_NONE;
		sink_fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
		sink_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	}

	for (pad = RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN;
	     pad <= RKISP2_ISP_PAD_SOURCE_VIDEO_SELF; pad++) {
		src_fmt = v4l2_subdev_state_get_format(sd_state, pad);
		*src_fmt = *sink_fmt;
		src_fmt->code = RKISP2_DEF_SRC_PAD_FMT;
		src_fmt->colorspace = V4L2_COLORSPACE_SRGB;
		src_fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
		src_fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
		src_fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	}

	return 0;
}

static void rkisp2_isp_set_src_fmt(struct rkisp2_isp *isp,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_mbus_framefmt *format,
				   u32 pad)
{
	const struct rkisp2_mbus_info *sink_info;
	const struct rkisp2_mbus_info *src_info;
	struct v4l2_mbus_framefmt *sink_fmt;
	struct v4l2_mbus_framefmt *src_fmt;
	bool set_csc;

	/* All the video sink formats and crops are kept equal */
	sink_fmt = v4l2_subdev_state_get_format(sd_state,
						RKISP2_ISP_PAD_SINK_VIDEO_DMA_0);
	src_fmt = v4l2_subdev_state_get_format(sd_state, pad);

	/*
	 * Media bus code. The ISP can operate in pass-through mode (Bayer in,
	 * Bayer out or YUV in, YUV out) or process Bayer data to YUV, but
	 * can't convert from YUV to Bayer.
	 */
	sink_info = rkisp2_mbus_info_get_by_code(sink_fmt->code);

	src_fmt->code = format->code;
	src_info = rkisp2_mbus_info_get_by_code(src_fmt->code);
	if (!src_info || !(src_info->direction & RKISP2_ISP_SD_SRC)) {
		src_fmt->code = RKISP2_DEF_SRC_PAD_FMT;
		src_info = rkisp2_mbus_info_get_by_code(src_fmt->code);
	}

	if (sink_info->pixel_enc == V4L2_PIXEL_ENC_YUV &&
	    src_info->pixel_enc == V4L2_PIXEL_ENC_BAYER) {
		src_fmt->code = sink_fmt->code;
		src_info = sink_info;
	}

	/*
	 * TODO Set source size = sink size if raw mode and source size > sink
	 * size as there is no resizer in raw mode. source size < sink size is
	 * fine because dual crop works in raw mode, unless we're on hardware
	 * with no dual crop in which case this needs to be adjusted as well.
	 */

	src_fmt->width = clamp_t(u32, format->width,
				 RKISP2_ISP_MIN_WIDTH,
				 isp->rkisp2->info->max_width);
	src_fmt->height = clamp_t(u32, format->height,
				  RKISP2_ISP_MIN_HEIGHT,
				  isp->rkisp2->info->max_height);

	/*
	 * Copy the color space for the sink pad. When converting from Bayer to
	 * YUV, default to a limited quantization range.
	 */
	src_fmt->colorspace = sink_fmt->colorspace;
	src_fmt->xfer_func = sink_fmt->xfer_func;
	src_fmt->ycbcr_enc = sink_fmt->ycbcr_enc;

	if (sink_info->pixel_enc == V4L2_PIXEL_ENC_BAYER &&
	    src_info->pixel_enc == V4L2_PIXEL_ENC_YUV)
		src_fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	else
		src_fmt->quantization = sink_fmt->quantization;

	/*
	 * Allow setting the source color space fields when the SET_CSC flag is
	 * set and the source format is YUV. If the sink format is YUV, don't
	 * set the color primaries, transfer function or YCbCr encoding as the
	 * ISP is bypassed in that case and passes YUV data through without
	 * modifications.
	 *
	 * The color primaries and transfer function are configured through the
	 * cross-talk matrix and tone curve respectively. Settings for those
	 * hardware blocks are conveyed through the ISP parameters buffer, as
	 * they need to combine color space information with other image tuning
	 * characteristics and can't thus be computed by the kernel based on the
	 * color space. The source pad colorspace and xfer_func fields are thus
	 * ignored by the driver, but can be set by userspace to propagate
	 * accurate color space information down the pipeline.
	 */
	set_csc = format->flags & V4L2_MBUS_FRAMEFMT_SET_CSC;

	if (set_csc && src_info->pixel_enc == V4L2_PIXEL_ENC_YUV) {
		if (sink_info->pixel_enc == V4L2_PIXEL_ENC_BAYER) {
			if (format->colorspace != V4L2_COLORSPACE_DEFAULT)
				src_fmt->colorspace = format->colorspace;
			if (format->xfer_func != V4L2_XFER_FUNC_DEFAULT)
				src_fmt->xfer_func = format->xfer_func;
			if (format->ycbcr_enc != V4L2_YCBCR_ENC_DEFAULT)
				src_fmt->ycbcr_enc = format->ycbcr_enc;
		}

		if (format->quantization != V4L2_QUANTIZATION_DEFAULT)
			src_fmt->quantization = format->quantization;
	}

	*format = *src_fmt;

	/*
	 * Restore the SET_CSC flag if it was set to indicate support for the
	 * CSC setting API.
	 */
	if (set_csc)
		format->flags |= V4L2_MBUS_FRAMEFMT_SET_CSC;
}

static void rkisp2_isp_set_sink_fmt(struct rkisp2_isp *isp,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_mbus_framefmt *format)
{
	const struct rkisp2_mbus_info *mbus_info;
	struct v4l2_mbus_framefmt *sink_fmt;
	bool is_yuv;
	enum rkisp2_isp_pad pad;

	sink_fmt = v4l2_subdev_state_get_format(sd_state, RKISP2_ISP_PAD_SINK_VIDEO_DMA_0);
	sink_fmt->code = format->code;
	mbus_info = rkisp2_mbus_info_get_by_code(sink_fmt->code);
	if (!mbus_info || !(mbus_info->direction & RKISP2_ISP_SD_SINK)) {
		sink_fmt->code = RKISP2_DEF_SINK_PAD_FMT;
		mbus_info = rkisp2_mbus_info_get_by_code(sink_fmt->code);
	}

	sink_fmt->width = clamp_t(u32, format->width,
				  RKISP2_ISP_MIN_WIDTH,
				  isp->rkisp2->info->max_width);
	sink_fmt->height = clamp_t(u32, format->height,
				   RKISP2_ISP_MIN_HEIGHT,
				   isp->rkisp2->info->max_height);

	/*
	 * Adjust the color space fields. Accept any color primaries and
	 * transfer function for both YUV and Bayer. For YUV any YCbCr encoding
	 * and quantization range is also accepted. For Bayer formats, the YCbCr
	 * encoding isn't applicable, and the quantization range can only be
	 * full.
	 */
	is_yuv = mbus_info->pixel_enc == V4L2_PIXEL_ENC_YUV;

	sink_fmt->colorspace = format->colorspace ? :
			       (is_yuv ? V4L2_COLORSPACE_SRGB :
				V4L2_COLORSPACE_RAW);
	sink_fmt->xfer_func = format->xfer_func ? :
			      V4L2_MAP_XFER_FUNC_DEFAULT(sink_fmt->colorspace);
	if (is_yuv) {
		sink_fmt->ycbcr_enc = format->ycbcr_enc ? :
			V4L2_MAP_YCBCR_ENC_DEFAULT(sink_fmt->colorspace);
		sink_fmt->quantization = format->quantization ? :
			V4L2_MAP_QUANTIZATION_DEFAULT(false, sink_fmt->colorspace,
						      sink_fmt->ycbcr_enc);
	} else {
		/*
		 * The YCbCr encoding isn't applicable for non-YUV formats, but
		 * V4L2 has no "no encoding" value. Hardcode it to Rec. 601, it
		 * should be ignored by userspace.
		 */
		sink_fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
		sink_fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	}

	*format = *sink_fmt;

	/* Propagate to the other video sink pads */
	for (pad = RKISP2_ISP_PAD_SINK_VIDEO_DMA_1;
	     pad < RKISP2_ISP_PAD_SINK_VIDEO_DMA_MAX; pad++) {
		sink_fmt = v4l2_subdev_state_get_format(sd_state, pad);
		*sink_fmt = *format;
	}
}

static int rkisp2_isp_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state,
			      struct v4l2_subdev_format *fmt)
{
	struct rkisp2_isp *isp = to_rkisp2_isp(sd);

	if (fmt->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_0 ||
	    fmt->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_1 ||
	    fmt->pad == RKISP2_ISP_PAD_SINK_VIDEO_DMA_2)
		rkisp2_isp_set_sink_fmt(isp, sd_state, &fmt->format);
	else if (fmt->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN ||
		 fmt->pad == RKISP2_ISP_PAD_SOURCE_VIDEO_SELF)
		rkisp2_isp_set_src_fmt(isp, sd_state, &fmt->format, fmt->pad);
	else
		fmt->format = *v4l2_subdev_state_get_format(sd_state,
							    fmt->pad);

	return 0;
}

static int rkisp2_subdev_link_validate(struct media_link *link)
{
	return v4l2_subdev_link_validate(link);
}

static const struct v4l2_subdev_pad_ops rkisp2_isp_pad_ops = {
	.enum_mbus_code = rkisp2_isp_enum_mbus_code,
	.enum_frame_size = rkisp2_isp_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = rkisp2_isp_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
};

/* ----------------------------------------------------------------------------
 * Stream operations
 */

static int rkisp2_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct rkisp2_isp *isp = to_rkisp2_isp(sd);
	struct rkisp2_device *rkisp2 = isp->rkisp2;
	struct v4l2_subdev_state *sd_state;
	struct media_pad *source_pad = NULL;
	struct media_pad *sink_pad = NULL;
	int ret;

	if (!enable) {
		if (rkisp2->source)
			v4l2_subdev_call(rkisp2->source, video, s_stream,
					 false);
		rkisp2_isp_stop(isp);
		return 0;
	}

	sink_pad = &isp->pads[rkisp2_isp_get_active_sink_pad(isp)];
	source_pad = media_pad_remote_pad_unique(sink_pad);
	if (IS_ERR(source_pad)) {
		dev_dbg(rkisp2->dev, "Failed to get source for ISP: %ld\n",
			PTR_ERR(source_pad));
		return -EPIPE;
	}

	if (is_media_entity_v4l2_subdev(source_pad->entity)) {
		rkisp2->source =
			media_entity_to_v4l2_subdev(source_pad->entity);
		if (!rkisp2->source) {
			/* This should really not happen, so is not worth a message. */
			return -EPIPE;
		}
	} else {
		dev_dbg(rkisp2->dev, "ISP source is not a v4l2 subdev\n");
		rkisp2->source = NULL;
	}

	isp->frame_sequence = -1;

	sd_state = v4l2_subdev_lock_and_get_active_state(sd);

	ret = rkisp2_config_cif(isp, sd_state);
	if (ret)
		goto out_unlock;

	ret = rkisp2_isp_start(isp, sd_state, source_pad);
	if (ret)
		goto out_unlock;

	if (rkisp2->source) {
		ret = v4l2_subdev_call(rkisp2->source, video, s_stream, true);
		if (ret) {
			rkisp2_isp_stop(isp);
			goto out_unlock;
		}
	}

out_unlock:
	v4l2_subdev_unlock_state(sd_state);
	return ret;
}

static int rkisp2_isp_subs_evt(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			       struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static const struct media_entity_operations rkisp2_isp_media_ops = {
	.link_validate = rkisp2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops rkisp2_isp_video_ops = {
	.s_stream = rkisp2_isp_s_stream,
};

//TODO: add .s_power?
static const struct v4l2_subdev_core_ops rkisp2_isp_core_ops = {
	.subscribe_event = rkisp2_isp_subs_evt,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops rkisp2_isp_ops = {
	.core = &rkisp2_isp_core_ops,
	.video = &rkisp2_isp_video_ops,
	.pad = &rkisp2_isp_pad_ops,
};

static const struct v4l2_subdev_internal_ops rkisp2_isp_internal_ops = {
	.init_state = rkisp2_isp_init_state,
};

int rkisp2_isp_register(struct rkisp2_device *rkisp2)
{
	struct rkisp2_isp *isp = &rkisp2->isp;
	struct media_pad *pads = isp->pads;
	struct v4l2_subdev *sd = &isp->sd;
	int ret;

	isp->rkisp2 = rkisp2;

	v4l2_subdev_init(sd, &rkisp2_isp_ops);
	sd->internal_ops = &rkisp2_isp_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->entity.ops = &rkisp2_isp_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	sd->owner = THIS_MODULE;
	strscpy(sd->name, RKISP2_ISP_DEV_NAME, sizeof(sd->name));

	pads[RKISP2_ISP_PAD_SINK_VIDEO_DMA_0].flags = MEDIA_PAD_FL_SINK;
	pads[RKISP2_ISP_PAD_SINK_VIDEO_DMA_1].flags = MEDIA_PAD_FL_SINK;
	pads[RKISP2_ISP_PAD_SINK_VIDEO_DMA_2].flags = MEDIA_PAD_FL_SINK;
	pads[RKISP2_ISP_PAD_SINK_VIDEO_CIF].flags = MEDIA_PAD_FL_SINK;
	pads[RKISP2_ISP_PAD_SINK_PARAMS].flags = MEDIA_PAD_FL_SINK;
	pads[RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN].flags = MEDIA_PAD_FL_SOURCE;
	pads[RKISP2_ISP_PAD_SOURCE_VIDEO_SELF].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, RKISP2_ISP_PAD_MAX, pads);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_subdev_cleanup;

	ret = v4l2_device_register_subdev(&rkisp2->v4l2_dev, sd);
	if (ret) {
		dev_err(rkisp2->dev, "Failed to register isp subdev\n");
		goto err_subdev_cleanup;
	}

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(sd);
err_entity_cleanup:
	media_entity_cleanup(&sd->entity);
	isp->sd.v4l2_dev = NULL;
	return ret;
}

void rkisp2_isp_unregister(struct rkisp2_device *rkisp2)
{
	struct rkisp2_isp *isp = &rkisp2->isp;

	if (!isp->sd.v4l2_dev)
		return;

	v4l2_device_unregister_subdev(&isp->sd);
	v4l2_subdev_cleanup(&isp->sd);
	media_entity_cleanup(&isp->sd.entity);
}

/* ----------------------------------------------------------------------------
 * Interrupt handlers
 */

static void rkisp2_isp_queue_event_sof(struct rkisp2_isp *isp)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};

	event.u.frame_sync.frame_sequence = isp->frame_sequence;
	v4l2_event_queue(isp->sd.devnode, &event);
}

irqreturn_t rkisp2_isp_isr(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct rkisp2_device *rkisp2 = dev_get_drvdata(dev);
	u32 status, isp_err;

	if (!rkisp2->irqs_enabled)
		return IRQ_NONE;

	status = rkisp2_read(rkisp2, RKISP2_CIF_ISP_MIS);
	if (!status)
		return IRQ_NONE;

	/* Vertical sync signal, starting generating new frame */
	if (status & RKISP2_CIF_ISP_V_START) {
		rkisp2->isp.frame_sequence++;
		rkisp2_isp_queue_event_sof(&rkisp2->isp);
		if (status & RKISP2_CIF_ISP_FRAME) {
			WARN_ONCE(1, "irq delay is too long, buffers might not be in sync\n");
			rkisp2->debug.irq_delay++;
		}
	}
	if (status & RKISP2_CIF_ISP_PIC_SIZE_ERROR) {
		/* Clear pic_size_error */
		isp_err = rkisp2_read(rkisp2, RKISP2_CIF_ISP_ERR);
		if (isp_err & RKISP2_CIF_ISP_ERR_INFORM_SIZE)
			rkisp2->debug.inform_size_error++;
		if (isp_err & RKISP2_CIF_ISP_ERR_IS_SIZE)
			rkisp2->debug.img_stabilization_size_error++;
		if (isp_err & RKISP2_CIF_ISP_ERR_OUTFORM_SIZE)
			rkisp2->debug.outform_size_error++;
		rkisp2_write(rkisp2, RKISP2_CIF_ISP_ERR_CLR, isp_err);
	} else if (status & RKISP2_CIF_ISP_DATA_LOSS) {
		/* keep track of data_loss in debugfs */
		rkisp2->debug.data_loss++;
	}

	if (status & RKISP2_CIF_ISP_FRAME) {
		rkisp2->debug.complete_frames++;
		rkisp2_params_isr(&rkisp2->params);
	}

	rkisp2_write(rkisp2, RKISP2_CIF_ISP_ICR, status);

	return IRQ_HANDLED;
}
