// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - V4L2 capture device
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>

#include "rkisp2-common.h"

#define RKISP2_SP_DEV_NAME	RKISP2_DRIVER_NAME "_selfpath"
#define RKISP2_MP_DEV_NAME	RKISP2_DRIVER_NAME "_mainpath"

enum rkisp2_plane {
	RKISP2_PLANE_Y	= 0,
	RKISP2_PLANE_CB	= 1,
	RKISP2_PLANE_CR	= 2
};

/*
 * @fourcc: pixel format
 * @fmt_type: helper filed for pixel format
 * @uv_swap: if cb cr swapped, for yuv
 * @write_format: defines how YCbCr self picture data is written to memory
 * @output_format: defines the output format (RKISP2_CIF_MI_INIT_MP_OUTPUT_* for
 *	the main path and RKISP2_MI_CTRL_SP_OUTPUT_* for the self path)
 * @mbus: the mbus code associated with the pixel format
 */
struct rkisp2_capture_fmt_cfg {
	u32 fourcc;
	u32 uv_swap : 1;
	u32 write_format;
	u32 output_format;
	u32 mbus;
};

struct rkisp2_capture_ops {
	void (*config)(struct rkisp2_capture *cap);
	void (*stop)(struct rkisp2_capture *cap);
	void (*enable)(struct rkisp2_capture *cap);
	void (*disable)(struct rkisp2_capture *cap);
	void (*set_data_path)(struct rkisp2_capture *cap);
	bool (*is_stopped)(struct rkisp2_capture *cap);
};

struct rkisp2_capture_config {
	const struct rkisp2_capture_fmt_cfg *fmts;
	int fmt_size;
	struct {
		u32 y_size_init;
		u32 cb_size_init;
		u32 cr_size_init;
		u32 y_base_ad_init;
		u32 cb_base_ad_init;
		u32 cr_base_ad_init;
		u32 y_offs_cnt_init;
		u32 cb_offs_cnt_init;
		u32 cr_offs_cnt_init;
		u32 y_base_ad_shd;
		u32 length;
		u32 ctrl;
		u32 y_pic_size;
	} mi;
};

/*
 * The supported pixel formats for mainpath. NOTE, pixel formats with identical 'mbus'
 * are grouped together. This is assumed and used by the function rkisp2_cap_enum_mbus_codes
 */
static const struct rkisp2_capture_fmt_cfg rkisp2_mp_fmts[] = {
	/* yuv422 */
	/*
	 * Y and U/V are swapped, and there doesn't seem to be a register that
	 * allows swapping them like the i.MX8MP did, so we only support UYVY
	 * and not YUYV for now.
	 */
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUVINT,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_PLA_OR_RAW8,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV61,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16M,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV61M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU422M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_PLA_OR_RAW8,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	},
	/* yuv400 */
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_PLA_OR_RAW8,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV400,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	},
	/* yuv420 */
	{
		.fourcc = V4L2_PIX_FMT_NV21,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = ISP32_MI_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = ISP32_MI_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV21M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12M,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_SPLA,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV420,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_PLA_OR_RAW8,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU420,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_MP_WRITE_YUV_PLA_OR_RAW8,
		.output_format = RKISP2_CIF_MI_INIT_MP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	},
};

/*
 * The supported pixel formats for selfpath. NOTE, pixel formats with identical 'mbus'
 * are grouped together. This is assumed and used by the function rkisp2_cap_enum_mbus_codes
 */
static const struct rkisp2_capture_fmt_cfg rkisp2_sp_fmts[] = {
	/* yuv422 */
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_INT,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV61,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16M,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV61M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU422M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	},
	/* yuv400 */
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV422,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	},
	/* rgb */
	{
		.fourcc = V4L2_PIX_FMT_XBGR32,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_RGB888,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_RGB565,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_RGB565,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	},
	/* yuv420 */
	{
		.fourcc = V4L2_PIX_FMT_NV21,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_2X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV21M,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12M,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_SPLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV420,
		.uv_swap = 0,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU420,
		.uv_swap = 1,
		.write_format = RKISP2_MI_CTRL_SP_WRITE_PLA,
		.output_format = RKISP2_MI_CTRL_SP_OUTPUT_YUV420,
		.mbus = MEDIA_BUS_FMT_YUYV8_1_5X8,
	},
};

static const struct rkisp2_capture_config rkisp2_capture_config_mp = {
	.fmts = rkisp2_mp_fmts,
	.fmt_size = ARRAY_SIZE(rkisp2_mp_fmts),
	.mi = {
		.y_size_init =		RKISP2_CIF_MI_MP_Y_SIZE_INIT,
		.cb_size_init =		RKISP2_CIF_MI_MP_CB_SIZE_INIT,
		.cr_size_init =		RKISP2_CIF_MI_MP_CR_SIZE_INIT,
		.y_base_ad_init =	RKISP2_CIF_MI_MP_Y_BASE_AD_INIT,
		.cb_base_ad_init =	RKISP2_CIF_MI_MP_CB_BASE_AD_INIT,
		.cr_base_ad_init =	RKISP2_CIF_MI_MP_CR_BASE_AD_INIT,
		.y_offs_cnt_init =	RKISP2_CIF_MI_MP_Y_OFFS_CNT_INIT,
		.cb_offs_cnt_init =	RKISP2_CIF_MI_MP_CB_OFFS_CNT_INIT,
		.cr_offs_cnt_init =	RKISP2_CIF_MI_MP_CR_OFFS_CNT_INIT,
		.y_base_ad_shd =	RKISP2_CIF_MI_MP_Y_BASE_AD_SHD,
		.y_pic_size =		ISP3X_MI_MP_WR_Y_PIC_SIZE,
	},
};

static const struct rkisp2_capture_config rkisp2_capture_config_sp = {
	.fmts = rkisp2_sp_fmts,
	.fmt_size = ARRAY_SIZE(rkisp2_sp_fmts),
	.mi = {
		.y_size_init =		RKISP2_CIF_MI_SP_Y_SIZE_INIT,
		.cb_size_init =		RKISP2_CIF_MI_SP_CB_SIZE_INIT,
		.cr_size_init =		RKISP2_CIF_MI_SP_CR_SIZE_INIT,
		.y_base_ad_init =	RKISP2_CIF_MI_SP_Y_BASE_AD_INIT,
		.cb_base_ad_init =	RKISP2_CIF_MI_SP_CB_BASE_AD_INIT,
		.cr_base_ad_init =	RKISP2_CIF_MI_SP_CR_BASE_AD_INIT,
		.y_offs_cnt_init =	RKISP2_CIF_MI_SP_Y_OFFS_CNT_INIT,
		.cb_offs_cnt_init =	RKISP2_CIF_MI_SP_CB_OFFS_CNT_INIT,
		.cr_offs_cnt_init =	RKISP2_CIF_MI_SP_CR_OFFS_CNT_INIT,
		.y_base_ad_shd =	RKISP2_CIF_MI_SP_Y_BASE_AD_SHD,
		.y_pic_size =		ISP3X_MI_SP_WR_Y_PIC_SIZE,
	},
};

static inline struct rkisp2_vdev_node *
rkisp2_vdev_to_node(struct video_device *vdev)
{
	return container_of(vdev, struct rkisp2_vdev_node, vdev);
}

int rkisp2_cap_enum_mbus_codes(struct rkisp2_capture *cap,
			       struct v4l2_subdev_mbus_code_enum *code)
{
	const struct rkisp2_capture_fmt_cfg *fmts = cap->config->fmts;
	/*
	 * initialize curr_mbus to non existing mbus code 0 to ensure it is
	 * different from fmts[0].mbus
	 */
	u32 curr_mbus = 0;
	int i, n = 0;

	for (i = 0; i < cap->config->fmt_size; i++) {
		if (fmts[i].mbus == curr_mbus)
			continue;

		curr_mbus = fmts[i].mbus;
		if (n++ == code->index) {
			code->code = curr_mbus;
			return 0;
		}
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------------
 * Stream operations for self-picture path (sp) and main-picture path (mp)
 */

static void rkisp2_mi_config_ctrl(struct rkisp2_capture *cap)
{
	u32 mi_ctrl = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL);

	mi_ctrl &= ~GENMASK(17, 16);
	mi_ctrl |= RKISP2_CIF_MI_CTRL_BURST_LEN_LUM_64;

	mi_ctrl &= ~GENMASK(19, 18);
	mi_ctrl |= RKISP2_CIF_MI_CTRL_BURST_LEN_CHROM_64;

	mi_ctrl |= RKISP2_CIF_MI_CTRL_INIT_BASE_EN |
		   RKISP2_CIF_MI_CTRL_INIT_OFFSET_EN;

	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static u32 rkisp2_pixfmt_comp_size(const struct v4l2_pix_format_mplane *pixm,
				   unsigned int component)
{
	/*
	 * If packed format, then plane_fmt[0].sizeimage is the sum of all
	 * components, so we need to calculate just the size of Y component.
	 * See rkisp2_fill_pixfmt().
	 */
	if (!component && pixm->num_planes == 1)
		return pixm->plane_fmt[0].bytesperline * pixm->height;
	return pixm->plane_fmt[component].sizeimage;
}

static void rkisp2_irq_frame_end_enable(struct rkisp2_capture *cap)
{
	u32 mi_imsc = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_IMSC);

	mi_imsc |= RKISP2_CIF_MI_FRAME(cap);
	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_IMSC, mi_imsc);
}

static void rkisp2_mp_config(struct rkisp2_capture *cap)
{
	const struct v4l2_pix_format_mplane *pixm = &cap->pix.fmt;
	struct rkisp2_device *rkisp2 = cap->rkisp2;
	u32 reg;

	rkisp2_write(rkisp2, cap->config->mi.y_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_Y));
	rkisp2_write(rkisp2, cap->config->mi.cb_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CB));
	rkisp2_write(rkisp2, cap->config->mi.cr_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CR));

	rkisp2_write(rkisp2, ISP3X_MI_MP_WR_Y_LLENGTH, cap->stride);
	rkisp2_write(rkisp2, ISP3X_MI_MP_WR_Y_PIC_WIDTH, pixm->width);
	rkisp2_write(rkisp2, ISP3X_MI_MP_WR_Y_PIC_HEIGHT, pixm->height);
	rkisp2_write(rkisp2, ISP3X_MI_MP_WR_Y_PIC_SIZE,
		     cap->stride * pixm->height);

	rkisp2_irq_frame_end_enable(cap);

	/* set uv swapping for semiplanar formats */
	if (cap->pix.info->comp_planes == 2) {
		reg = rkisp2_read(rkisp2, ISP3X_MI_WR_XTD_FORMAT_CTRL);
		if (cap->pix.cfg->uv_swap)
			reg |= ISP3X_MI_XTD_FORMAT_MP_UV_SWAP;
		else
			reg &= ~ISP3X_MI_XTD_FORMAT_MP_UV_SWAP;
		rkisp2_write(rkisp2, ISP3X_MI_WR_XTD_FORMAT_CTRL, reg);
	}

	/*
	 * 2.x and 3.x are both missing the register that allows swapping
	 * bytes, and at least on the rk3588 the bytes are output in UYVY
	 * order. Thus without byte swap, YUYV cannot be achieved.
	 */

	rkisp2_write(rkisp2, RKISP2_CIF_MI_INIT,
		     cap->pix.cfg->output_format);

	u32 mask = ISP3X_MPFBC_FORCE_UPD | ISP3X_MP_YUV_MODE;

	reg = rkisp2_read(rkisp2, ISP3X_MPFBC_CTRL) & ~mask;
	if (pixm->pixelformat == V4L2_PIX_FMT_NV21 ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV12 ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV21M ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV21M ||
	    pixm->pixelformat == V4L2_PIX_FMT_YVU420)
		reg |= ISP3X_SEPERATE_YUV_CFG;
	else
		reg |= ISP3X_SEPERATE_YUV_CFG | ISP3X_MP_YUV_MODE;
	rkisp2_write(rkisp2, ISP3X_MPFBC_CTRL, reg);

	rkisp2_mi_config_ctrl(cap);

	reg = rkisp2_read(rkisp2, RKISP2_CIF_MI_CTRL);
	reg &= ~RKISP2_MI_CTRL_MP_FMT_MASK;
	reg |= cap->pix.cfg->write_format | RKISP2_CIF_MI_MP_AUTOUPDATE_ENABLE;
	rkisp2_write(rkisp2, RKISP2_CIF_MI_CTRL, reg);
}

static void rkisp2_sp_config(struct rkisp2_capture *cap)
{
	const struct v4l2_pix_format_mplane *pixm = &cap->pix.fmt;
	struct rkisp2_device *rkisp2 = cap->rkisp2;
	u32 mi_ctrl, reg;

	rkisp2_write(rkisp2, cap->config->mi.y_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_Y));
	rkisp2_write(rkisp2, cap->config->mi.cb_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CB));
	rkisp2_write(rkisp2, cap->config->mi.cr_size_init,
		     rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CR));

	rkisp2_write(rkisp2, RKISP2_CIF_MI_SP_Y_LLENGTH, cap->stride);
	rkisp2_write(rkisp2, RKISP2_CIF_MI_SP_Y_PIC_WIDTH, pixm->width);
	rkisp2_write(rkisp2, RKISP2_CIF_MI_SP_Y_PIC_HEIGHT, pixm->height);
	rkisp2_write(rkisp2, RKISP2_CIF_MI_SP_Y_PIC_SIZE, cap->stride * pixm->height);

	rkisp2_irq_frame_end_enable(cap);

	/* set uv swapping for semiplanar formats */
	if (cap->pix.info->comp_planes == 2) {
		reg = rkisp2_read(rkisp2, ISP3X_MI_WR_XTD_FORMAT_CTRL);
		if (cap->pix.cfg->uv_swap)
			reg |= ISP3X_MI_XTD_FORMAT_SP_UV_SWAP;
		else
			reg &= ~ISP3X_MI_XTD_FORMAT_SP_UV_SWAP;
		rkisp2_write(rkisp2, ISP3X_MI_WR_XTD_FORMAT_CTRL, reg);
	}

	/*
	 * 2.x and 3.x are both missing the register that allows swapping
	 * bytes, and at least on the rk3588 the bytes are output in UYVY
	 * order. Thus without byte swap, YUYV cannot be achieved.
	 */

	u32 mask = ISP3X_MPFBC_FORCE_UPD | ISP3X_SP_YUV_MODE;

	reg = rkisp2_read(rkisp2, ISP3X_MPFBC_CTRL) & ~mask;
	if (pixm->pixelformat == V4L2_PIX_FMT_NV21 ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV12 ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV21M ||
	    pixm->pixelformat == V4L2_PIX_FMT_NV21M ||
	    pixm->pixelformat == V4L2_PIX_FMT_YVU420)
		reg |= ISP3X_SEPERATE_YUV_CFG;
	else
		reg |= ISP3X_SEPERATE_YUV_CFG | ISP3X_SP_YUV_MODE;
	rkisp2_write(rkisp2, ISP3X_MPFBC_CTRL, reg);

	rkisp2_mi_config_ctrl(cap);

	mi_ctrl = rkisp2_read(rkisp2, RKISP2_CIF_MI_CTRL);
	mi_ctrl &= ~RKISP2_MI_CTRL_SP_FMT_MASK;
	mi_ctrl |= cap->pix.cfg->write_format |
		   RKISP2_MI_CTRL_SP_INPUT_YUV422 |
		   cap->pix.cfg->output_format |
		   RKISP2_CIF_MI_SP_AUTOUPDATE_ENABLE;
	rkisp2_write(rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static void rkisp2_mp_disable(struct rkisp2_capture *cap)
{
	u32 mi_ctrl = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL);

	mi_ctrl &= ~(RKISP2_CIF_MI_CTRL_MP_ENABLE |
		     RKISP2_CIF_MI_CTRL_RAW_ENABLE);
	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static void rkisp2_sp_disable(struct rkisp2_capture *cap)
{
	u32 mi_ctrl = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL);

	mi_ctrl &= ~RKISP2_CIF_MI_CTRL_SP_ENABLE;
	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static void rkisp2_mp_enable(struct rkisp2_capture *cap)
{
	u32 mi_ctrl;

	rkisp2_mp_disable(cap);

	mi_ctrl = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL);
	if (v4l2_is_format_bayer(cap->pix.info))
		mi_ctrl |= RKISP2_CIF_MI_CTRL_RAW_ENABLE;
	/* YUV */
	else
		mi_ctrl |= RKISP2_CIF_MI_CTRL_MP_ENABLE;

	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static void rkisp2_sp_enable(struct rkisp2_capture *cap)
{
	u32 mi_ctrl = rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL);

	mi_ctrl |= RKISP2_CIF_MI_CTRL_SP_ENABLE;
	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_CTRL, mi_ctrl);
}

static void rkisp2_mp_sp_stop(struct rkisp2_capture *cap)
{
	if (!cap->is_streaming)
		return;
	rkisp2_write(cap->rkisp2, RKISP2_CIF_MI_ICR, RKISP2_CIF_MI_FRAME(cap));
	cap->ops->disable(cap);
}

static bool rkisp2_mp_is_stopped(struct rkisp2_capture *cap)
{
	u32 en = RKISP2_CIF_MI_CTRL_SHD_MP_IN_ENABLED |
		 RKISP2_CIF_MI_CTRL_SHD_RAW_OUT_ENABLED;

	return !(rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL_SHD) & en);
}

static bool rkisp2_sp_is_stopped(struct rkisp2_capture *cap)
{
	return !(rkisp2_read(cap->rkisp2, RKISP2_CIF_MI_CTRL_SHD) &
		 RKISP2_CIF_MI_CTRL_SHD_SP_IN_ENABLED);
}

static void rkisp2_mp_set_data_path(struct rkisp2_capture *cap)
{
	u32 dpcl = rkisp2_read(cap->rkisp2, RKISP2_CIF_VI_DPCL);

	dpcl = dpcl | RKISP2_CIF_VI_DPCL_CHAN_MODE_MP |
	       RKISP2_CIF_VI_DPCL_MP_MUX_MRSZ_MI;
	rkisp2_write(cap->rkisp2, RKISP2_CIF_VI_DPCL, dpcl);
}

static void rkisp2_sp_set_data_path(struct rkisp2_capture *cap)
{
	u32 dpcl = rkisp2_read(cap->rkisp2, RKISP2_CIF_VI_DPCL);

	dpcl |= RKISP2_CIF_VI_DPCL_CHAN_MODE_SP;
	rkisp2_write(cap->rkisp2, RKISP2_CIF_VI_DPCL, dpcl);
}

static const struct rkisp2_capture_ops rkisp2_capture_ops_mp = {
	.config = rkisp2_mp_config,
	.enable = rkisp2_mp_enable,
	.disable = rkisp2_mp_disable,
	.stop = rkisp2_mp_sp_stop,
	.set_data_path = rkisp2_mp_set_data_path,
	.is_stopped = rkisp2_mp_is_stopped,
};

static const struct rkisp2_capture_ops rkisp2_capture_ops_sp = {
	.config = rkisp2_sp_config,
	.enable = rkisp2_sp_enable,
	.disable = rkisp2_sp_disable,
	.stop = rkisp2_mp_sp_stop,
	.set_data_path = rkisp2_sp_set_data_path,
	.is_stopped = rkisp2_sp_is_stopped,
};

/* ----------------------------------------------------------------------------
 * Frame buffer operations
 */

static int rkisp2_dummy_buf_create(struct rkisp2_capture *cap)
{
	const struct v4l2_pix_format_mplane *pixm = &cap->pix.fmt;
	struct rkisp2_dummy_buffer *dummy_buf = &cap->buf.dummy;

	dummy_buf->size = max3(rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_Y),
			       rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CB),
			       rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CR));

	/* The driver never access vaddr, no mapping is required */
	dummy_buf->vaddr = dma_alloc_attrs(cap->rkisp2->dev,
					   dummy_buf->size,
					   &dummy_buf->dma_addr,
					   GFP_KERNEL,
					   DMA_ATTR_NO_KERNEL_MAPPING);
	if (!dummy_buf->vaddr)
		return -ENOMEM;

	return 0;
}

static void rkisp2_dummy_buf_destroy(struct rkisp2_capture *cap)
{
	dma_free_attrs(cap->rkisp2->dev,
		       cap->buf.dummy.size, cap->buf.dummy.vaddr,
		       cap->buf.dummy.dma_addr, DMA_ATTR_NO_KERNEL_MAPPING);
}

static void rkisp2_set_next_buf(struct rkisp2_capture *cap)
{
	cap->buf.curr = cap->buf.next;
	cap->buf.next = NULL;

	if (!list_empty(&cap->buf.queue)) {
		dma_addr_t *buff_addr;

		cap->buf.next = list_first_entry(&cap->buf.queue, struct rkisp2_buffer, queue);
		list_del(&cap->buf.next->queue);

		buff_addr = cap->buf.next->buff_addr;

		rkisp2_write(cap->rkisp2, cap->config->mi.y_base_ad_init,
			     buff_addr[RKISP2_PLANE_Y]);
		/*
		 * In order to support grey format we capture
		 * YUV422 planar format from the camera and
		 * set the U and V planes to the dummy buffer
		 */
		if (cap->pix.cfg->fourcc == V4L2_PIX_FMT_GREY) {
			rkisp2_write(cap->rkisp2,
				     cap->config->mi.cb_base_ad_init,
				     cap->buf.dummy.dma_addr);
		} else {
			rkisp2_write(cap->rkisp2,
				     cap->config->mi.cb_base_ad_init,
				     buff_addr[RKISP2_PLANE_CB]);
		}
	} else {
		/*
		 * Use the dummy space allocated by dma_alloc_coherent to
		 * throw data if there is no available buffer.
		 */
		rkisp2_write(cap->rkisp2, cap->config->mi.y_base_ad_init,
			     cap->buf.dummy.dma_addr);
		rkisp2_write(cap->rkisp2, cap->config->mi.cb_base_ad_init,
			     cap->buf.dummy.dma_addr);
	}

	/* Set plane offsets */
	rkisp2_write(cap->rkisp2, cap->config->mi.y_offs_cnt_init, 0);
	rkisp2_write(cap->rkisp2, cap->config->mi.cb_offs_cnt_init, 0);
	rkisp2_write(cap->rkisp2, cap->config->mi.cr_offs_cnt_init, 0);
}

/*
 * This function is called when a frame end comes. The next frame
 * is processing and we should set up buffer for next-next frame,
 * otherwise it will overflow.
 */
static void rkisp2_handle_buffer(struct rkisp2_capture *cap)
{
	struct rkisp2_isp *isp = &cap->rkisp2->isp;
	struct rkisp2_buffer *curr_buf;

	spin_lock(&cap->buf.lock);
	curr_buf = cap->buf.curr;

	if (curr_buf) {
		curr_buf->vb.sequence = isp->frame_sequence;
		curr_buf->vb.vb2_buf.timestamp = ktime_get_boottime_ns();
		curr_buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&curr_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	} else {
		cap->rkisp2->debug.frame_drop[cap->id]++;
	}

	rkisp2_set_next_buf(cap);
	spin_unlock(&cap->buf.lock);
}

irqreturn_t rkisp2_capture_isr(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct rkisp2_device *rkisp2 = dev_get_drvdata(dev);
	unsigned int i;
	u32 status;

	if (!rkisp2->irqs_enabled)
		return IRQ_NONE;

	status = rkisp2_read(rkisp2, RKISP2_CIF_MI_MIS);
	if (!status)
		return IRQ_NONE;

	for (i = 0; i < 2; ++i) {
		struct rkisp2_capture *cap = &rkisp2->capture_devs[i];

		if (!(status & RKISP2_CIF_MI_FRAME(cap)))
			continue;
		if (!cap->is_stopping) {
			rkisp2_handle_buffer(cap);
			continue;
		}
		/*
		 * Make sure stream is actually stopped, whose state
		 * can be read from the shadow register, before
		 * wake_up() thread which would immediately free all
		 * frame buffers. stop() takes effect at the next
		 * frame end that sync the configurations to shadow
		 * regs.
		 */
		if (!cap->ops->is_stopped(cap)) {
			cap->ops->stop(cap);
			continue;
		}
		cap->is_stopping = false;
		cap->is_streaming = false;
		wake_up(&cap->done);
	}

	rkisp2_dmarx_isr(rkisp2, status);

	rkisp2_write(rkisp2, RKISP2_CIF_MI_ICR, status);

	return IRQ_HANDLED;
}

/* ----------------------------------------------------------------------------
 * Vb2 operations
 */

static int rkisp2_vb2_queue_setup(struct vb2_queue *queue,
				  unsigned int *num_buffers,
				  unsigned int *num_planes,
				  unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct rkisp2_capture *cap = queue->drv_priv;
	const struct v4l2_pix_format_mplane *pixm = &cap->pix.fmt;
	unsigned int i;

	if (*num_planes) {
		if (*num_planes != pixm->num_planes)
			return -EINVAL;

		for (i = 0; i < pixm->num_planes; i++)
			if (sizes[i] < pixm->plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*num_planes = pixm->num_planes;
		for (i = 0; i < pixm->num_planes; i++)
			sizes[i] = pixm->plane_fmt[i].sizeimage;
	}

	return 0;
}

static int rkisp2_vb2_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_buffer *ispbuf =
		container_of(vbuf, struct rkisp2_buffer, vb);
	struct rkisp2_capture *cap = vb->vb2_queue->drv_priv;
	const struct v4l2_pix_format_mplane *pixm = &cap->pix.fmt;
	unsigned int i;

	memset(ispbuf->buff_addr, 0, sizeof(ispbuf->buff_addr));
	for (i = 0; i < pixm->num_planes; i++)
		ispbuf->buff_addr[i] = vb2_dma_contig_plane_dma_addr(vb, i);

	/* Convert to non-MPLANE */
	if (pixm->num_planes == 1) {
		ispbuf->buff_addr[RKISP2_PLANE_CB] =
			ispbuf->buff_addr[RKISP2_PLANE_Y] +
			rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_Y);
		ispbuf->buff_addr[RKISP2_PLANE_CR] =
			ispbuf->buff_addr[RKISP2_PLANE_CB] +
			rkisp2_pixfmt_comp_size(pixm, RKISP2_PLANE_CB);
	}

	/*
	 * uv swap can be supported for planar formats by switching
	 * the address of cb and cr
	 */
	if (cap->pix.info->comp_planes == 3 && cap->pix.cfg->uv_swap)
		swap(ispbuf->buff_addr[RKISP2_PLANE_CR],
		     ispbuf->buff_addr[RKISP2_PLANE_CB]);
	return 0;
}

static void rkisp2_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_buffer *ispbuf =
		container_of(vbuf, struct rkisp2_buffer, vb);
	struct rkisp2_capture *cap = vb->vb2_queue->drv_priv;

	spin_lock_irq(&cap->buf.lock);
	list_add_tail(&ispbuf->queue, &cap->buf.queue);
	spin_unlock_irq(&cap->buf.lock);
}

static int rkisp2_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct rkisp2_capture *cap = vb->vb2_queue->drv_priv;
	unsigned int i;

	for (i = 0; i < cap->pix.fmt.num_planes; i++) {
		unsigned long size = cap->pix.fmt.plane_fmt[i].sizeimage;

		if (vb2_plane_size(vb, i) < size) {
			dev_err(cap->rkisp2->dev,
				"User buffer too small (%ld < %ld)\n",
				vb2_plane_size(vb, i), size);
			return -EINVAL;
		}
		vb2_set_plane_payload(vb, i, size);
	}

	return 0;
}

static void rkisp2_return_all_buffers(struct rkisp2_capture *cap,
				      enum vb2_buffer_state state)
{
	struct rkisp2_buffer *buf;

	spin_lock_irq(&cap->buf.lock);
	if (cap->buf.curr) {
		vb2_buffer_done(&cap->buf.curr->vb.vb2_buf, state);
		cap->buf.curr = NULL;
	}
	if (cap->buf.next) {
		vb2_buffer_done(&cap->buf.next->vb.vb2_buf, state);
		cap->buf.next = NULL;
	}
	while (!list_empty(&cap->buf.queue)) {
		buf = list_first_entry(&cap->buf.queue,
				       struct rkisp2_buffer, queue);
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irq(&cap->buf.lock);
}

/*
 * Most registers inside the rockchip ISP2 have shadow register since
 * they must not be changed while processing a frame.
 * Usually, each sub-module updates its shadow register after
 * processing the last pixel of a frame.
 */
static void rkisp2_cap_stream_enable(struct rkisp2_capture *cap)
{
	struct rkisp2_device *rkisp2 = cap->rkisp2;
	struct rkisp2_capture *other = &rkisp2->capture_devs[cap->id ^ 1];

	cap->ops->set_data_path(cap);
	cap->ops->config(cap);

	/* Setup a buffer for the next frame */
	spin_lock_irq(&cap->buf.lock);
	rkisp2_set_next_buf(cap);
	cap->ops->enable(cap);

	/*
	 * It's safe to configure ACTIVE and SHADOW registers for the first
	 * stream. While when the second is starting, do NOT force update
	 * because it also updates the first one.
	 *
	 * The latter case would drop one more buffer(that is 2) since there's
	 * no buffer in a shadow register when the second FE received. This's
	 * also required because the second FE maybe corrupt especially when
	 * run at 120fps.
	 */
	if (!other->is_streaming) {
		u32 reg;

		/* Force cfg update.  */
		reg = rkisp2_read(rkisp2, RKISP2_CIF_MI_INIT)
		    & RKISP2_CIF_MI_INIT_MP_OUTPUT_MASK;

		reg |= RKISP2_CIF_MI_INIT_SOFT_UPD;
		rkisp2_write(rkisp2, RKISP2_CIF_MI_INIT, reg);

		rkisp2_set_next_buf(cap);
	}

	spin_unlock_irq(&cap->buf.lock);
	cap->is_streaming = true;
}

static void rkisp2_cap_stream_disable(struct rkisp2_capture *cap)
{
	int ret;

	/* Stream should stop in interrupt. If it doesn't, stop it by force. */
	cap->is_stopping = true;
	ret = wait_event_timeout(cap->done,
				 !cap->is_streaming,
				 msecs_to_jiffies(1000));
	if (!ret) {
		cap->rkisp2->debug.stop_timeout[cap->id]++;
		cap->ops->stop(cap);
		cap->is_stopping = false;
		cap->is_streaming = false;
	}
}

/*
 * rkisp2_pipeline_stream_disable - disable nodes in the pipeline
 *
 * Call s_stream(false) in the reverse order from
 * rkisp2_pipeline_stream_enable() and disable the DMA engine.
 * Should be called before video_device_pipeline_stop()
 */
static void rkisp2_pipeline_stream_disable(struct rkisp2_capture *cap)
	__must_hold(&cap->rkisp2->stream_lock)
{
	struct rkisp2_device *rkisp2 = cap->rkisp2;

	rkisp2_cap_stream_disable(cap);

	/*
	 * If the other capture is streaming, isp and sensor nodes shouldn't
	 * be disabled, skip them.
	 */
	if (rkisp2->pipe.start_count < (rkisp2->source ? 2 : 3))
		v4l2_subdev_call(&rkisp2->isp.sd, video, s_stream, false);

}

/*
 * rkisp2_pipeline_stream_enable - enable nodes in the pipeline
 *
 * Enable the DMA Engine and call s_stream(true) through the pipeline.
 * Should be called after video_device_pipeline_start()
 */
static int rkisp2_pipeline_stream_enable(struct rkisp2_capture *cap)
	__must_hold(&cap->rkisp2->stream_lock)
{
	struct rkisp2_device *rkisp2 = cap->rkisp2;
	int ret;

	rkisp2_cap_stream_enable(cap);

	/*
	 * TODO Validate the source pad formats and capture formats if one of
	 * the two is already streaming to prevent invalid combined main/self
	 * path configurations
	 */

	/*
	 * If the other capture is streaming, isp and sensor nodes are already
	 * enabled, skip them.
	 */
	if (rkisp2->pipe.start_count > 1)
		return 0;

	ret = v4l2_subdev_call(&rkisp2->isp.sd, video, s_stream, true);
	if (ret)
		goto err_disable_cap;

	return 0;

err_disable_cap:
	rkisp2_cap_stream_disable(cap);

	return ret;
}

static void rkisp2_vb2_stop_streaming(struct vb2_queue *queue)
{
	struct rkisp2_capture *cap = queue->drv_priv;
	struct rkisp2_vdev_node *node = &cap->vnode;
	struct rkisp2_device *rkisp2 = cap->rkisp2;

	dev_dbg(cap->rkisp2->dev, "%s, enter\n", __func__);

	mutex_lock(&cap->rkisp2->stream_lock);

	rkisp2_pipeline_stream_disable(cap);

	rkisp2_return_all_buffers(cap, VB2_BUF_STATE_ERROR);

	v4l2_pipeline_pm_put(&node->vdev.entity);
	pm_runtime_put(rkisp2->dev);

	rkisp2_dummy_buf_destroy(cap);

	video_device_pipeline_stop(&node->vdev);

	mutex_unlock(&cap->rkisp2->stream_lock);
}

static int
rkisp2_vb2_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct rkisp2_capture *cap = queue->drv_priv;
	struct media_entity *entity = &cap->vnode.vdev.entity;
	int ret;

	mutex_lock(&cap->rkisp2->stream_lock);

	ret = video_device_pipeline_start(&cap->vnode.vdev, &cap->rkisp2->pipe);
	if (ret) {
		dev_err(cap->rkisp2->dev, "start pipeline failed %d\n", ret);
		goto err_ret_buffers;
	}

	ret = rkisp2_dummy_buf_create(cap);
	if (ret)
		goto err_pipeline_stop;

	ret = pm_runtime_resume_and_get(cap->rkisp2->dev);
	if (ret < 0) {
		dev_err(cap->rkisp2->dev, "power up failed %d\n", ret);
		goto err_destroy_dummy;
	}
	ret = v4l2_pipeline_pm_get(entity);
	if (ret) {
		dev_err(cap->rkisp2->dev, "open cif pipeline failed %d\n", ret);
		goto err_pipe_pm_put;
	}

	ret = rkisp2_pipeline_stream_enable(cap);
	if (ret)
		goto err_v4l2_pm_put;

	mutex_unlock(&cap->rkisp2->stream_lock);

	return 0;

err_v4l2_pm_put:
	v4l2_pipeline_pm_put(entity);
err_pipe_pm_put:
	pm_runtime_put(cap->rkisp2->dev);
err_destroy_dummy:
	rkisp2_dummy_buf_destroy(cap);
err_pipeline_stop:
	video_device_pipeline_stop(&cap->vnode.vdev);
err_ret_buffers:
	rkisp2_return_all_buffers(cap, VB2_BUF_STATE_QUEUED);
	mutex_unlock(&cap->rkisp2->stream_lock);

	return ret;
}

static const struct vb2_ops rkisp2_vb2_ops = {
	.queue_setup = rkisp2_vb2_queue_setup,
	.buf_init = rkisp2_vb2_buf_init,
	.buf_queue = rkisp2_vb2_buf_queue,
	.buf_prepare = rkisp2_vb2_buf_prepare,
	.stop_streaming = rkisp2_vb2_stop_streaming,
	.start_streaming = rkisp2_vb2_start_streaming,
};

/* ----------------------------------------------------------------------------
 * IOCTLs operations
 */

static const struct v4l2_format_info *
rkisp2_fill_pixfmt(const struct rkisp2_capture *cap,
		   struct v4l2_pix_format_mplane *pixm)
{
	struct v4l2_plane_pix_format *plane_y = &pixm->plane_fmt[0];
	const struct v4l2_format_info *info;
	unsigned int i;
	u32 stride;

	memset(pixm->plane_fmt, 0, sizeof(pixm->plane_fmt));
	info = v4l2_format_info(pixm->pixelformat);
	pixm->num_planes = info->mem_planes;

	/*
	 * Clamp the stride to a reasonable value to avoid integer overflows
	 * when calculating the bytesperline and sizeimage values.
	 */
	stride = clamp(DIV_ROUND_UP(plane_y->bytesperline, info->bpp[0]),
		       pixm->width, 65536U);

	plane_y->bytesperline = stride * info->bpp[0];
	plane_y->sizeimage = plane_y->bytesperline * pixm->height;

	for (i = 1; i < info->comp_planes; i++) {
		struct v4l2_plane_pix_format *plane = &pixm->plane_fmt[i];

		/* bytesperline for other components derive from Y component */
		plane->bytesperline = DIV_ROUND_UP(stride, info->hdiv) *
				      info->bpp[i];
		plane->sizeimage = plane->bytesperline *
				   DIV_ROUND_UP(pixm->height, info->vdiv);
	}

	/*
	 * If pixfmt is packed, then plane_fmt[0] should contain the total size
	 * considering all components. plane_fmt[i] for i > 0 should be ignored
	 * by userspace as mem_planes == 1, but we are keeping information there
	 * for convenience.
	 */
	if (info->mem_planes == 1)
		for (i = 1; i < info->comp_planes; i++)
			plane_y->sizeimage += pixm->plane_fmt[i].sizeimage;

	return info;
}

static const struct rkisp2_capture_fmt_cfg *
rkisp2_find_fmt_cfg(const struct rkisp2_capture *cap, const u32 pixelfmt)
{
	unsigned int i;

	for (i = 0; i < cap->config->fmt_size; i++) {
		const struct rkisp2_capture_fmt_cfg *fmt = &cap->config->fmts[i];

		if (fmt->fourcc == pixelfmt)
			return &cap->config->fmts[i];
	}
	return NULL;
}

static void rkisp2_try_fmt(const struct rkisp2_capture *cap,
			   struct v4l2_pix_format_mplane *pixm,
			   const struct rkisp2_capture_fmt_cfg **fmt_cfg,
			   const struct v4l2_format_info **fmt_info)
{
	const struct rkisp2_capture_config *config = cap->config;
	const struct rkisp2_capture_fmt_cfg *fmt;
	const struct v4l2_format_info *info;
	static const unsigned int max_widths[] = {
		RKISP2_RSZ_MP_SRC_MAX_WIDTH, RKISP2_RSZ_SP_SRC_MAX_WIDTH
	};
	static const unsigned int max_heights[] = {
		RKISP2_RSZ_MP_SRC_MAX_HEIGHT, RKISP2_RSZ_SP_SRC_MAX_HEIGHT
	};

	fmt = rkisp2_find_fmt_cfg(cap, pixm->pixelformat);
	if (!fmt) {
		fmt = config->fmts;
		pixm->pixelformat = fmt->fourcc;
	}

	pixm->width = clamp_t(u32, pixm->width,
			      RKISP2_RSZ_SRC_MIN_WIDTH, max_widths[cap->id]);
	pixm->height = clamp_t(u32, pixm->height,
			       RKISP2_RSZ_SRC_MIN_HEIGHT, max_heights[cap->id]);

	pixm->field = V4L2_FIELD_NONE;
	pixm->colorspace = V4L2_COLORSPACE_DEFAULT;
	pixm->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pixm->quantization = V4L2_QUANTIZATION_DEFAULT;

	info = rkisp2_fill_pixfmt(cap, pixm);

	if (fmt_cfg)
		*fmt_cfg = fmt;
	if (fmt_info)
		*fmt_info = info;
}

static void rkisp2_set_fmt(struct rkisp2_capture *cap,
			   struct v4l2_pix_format_mplane *pixm)
{
	rkisp2_try_fmt(cap, pixm, &cap->pix.cfg, &cap->pix.info);

	cap->pix.fmt = *pixm;
	/*
	 * If you're having stride issues implementing this on new hardware,
	 * check here. For example, on rkisp 3.0 this is in bytes, but on rkisp
	 * 1.0 it's in pixels.
	 */
	cap->stride = pixm->plane_fmt[0].bytesperline;
}

static int rkisp2_enum_input(struct file *file, void *priv,
			     struct v4l2_input *input)
{
	if (input->index > 0)
		return -EINVAL;

	input->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(input->name, "Camera", sizeof(input->name));

	return 0;
}

static int rkisp2_try_fmt_vid_cap_mplane(struct file *file, void *fh,
					 struct v4l2_format *f)
{
	struct rkisp2_capture *cap = video_drvdata(file);

	rkisp2_try_fmt(cap, &f->fmt.pix_mp, NULL, NULL);

	return 0;
}

static int rkisp2_enum_fmt_vid_cap_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct rkisp2_capture *cap = video_drvdata(file);
	const struct rkisp2_capture_fmt_cfg *fmt = NULL;
	unsigned int i, n = 0;

	if (!f->mbus_code) {
		if (f->index >= cap->config->fmt_size)
			return -EINVAL;

		fmt = &cap->config->fmts[f->index];
		f->pixelformat = fmt->fourcc;
		return 0;
	}

	for (i = 0; i < cap->config->fmt_size; i++) {
		fmt = &cap->config->fmts[i];

		if (f->mbus_code && fmt->mbus != f->mbus_code)
			continue;

		if (n++ == f->index) {
			f->pixelformat = fmt->fourcc;
			return 0;
		}
	}
	return -EINVAL;
}

static int rkisp2_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	static const unsigned int max_widths[] = {
		RKISP2_RSZ_MP_SRC_MAX_WIDTH,
		RKISP2_RSZ_SP_SRC_MAX_WIDTH,
	};
	static const unsigned int max_heights[] = {
		RKISP2_RSZ_MP_SRC_MAX_HEIGHT,
		RKISP2_RSZ_SP_SRC_MAX_HEIGHT,
	};
	struct rkisp2_capture *cap = video_drvdata(file);

	if (fsize->index != 0)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;

	fsize->stepwise.min_width = RKISP2_RSZ_SRC_MIN_WIDTH;
	fsize->stepwise.max_width = max_widths[cap->id];
	fsize->stepwise.step_width = 2;

	fsize->stepwise.min_height = RKISP2_RSZ_SRC_MIN_HEIGHT;
	fsize->stepwise.max_height = max_heights[cap->id];
	fsize->stepwise.step_height = 2;

	return 0;
}

static int rkisp2_s_fmt_vid_cap_mplane(struct file *file,
				       void *priv, struct v4l2_format *f)
{
	struct rkisp2_capture *cap = video_drvdata(file);
	struct rkisp2_vdev_node *node =
				rkisp2_vdev_to_node(&cap->vnode.vdev);

	if (vb2_is_busy(&node->buf_queue))
		return -EBUSY;

	rkisp2_set_fmt(cap, &f->fmt.pix_mp);

	return 0;
}

static int rkisp2_g_fmt_vid_cap_mplane(struct file *file, void *fh,
				       struct v4l2_format *f)
{
	struct rkisp2_capture *cap = video_drvdata(file);

	f->fmt.pix_mp = cap->pix.fmt;

	return 0;
}

static int
rkisp2_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, RKISP2_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, RKISP2_DRIVER_NAME, sizeof(cap->card));
	strscpy(cap->bus_info, RKISP2_BUS_INFO, sizeof(cap->bus_info));

	return 0;
}

static const struct v4l2_ioctl_ops rkisp2_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_input = rkisp2_enum_input,
	.vidioc_try_fmt_vid_cap_mplane = rkisp2_try_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = rkisp2_s_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_cap_mplane = rkisp2_g_fmt_vid_cap_mplane,
	.vidioc_enum_fmt_vid_cap = rkisp2_enum_fmt_vid_cap_mplane,
	.vidioc_enum_framesizes = rkisp2_enum_framesizes,
	.vidioc_querycap = rkisp2_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int rkisp2_capture_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		media_entity_to_video_device(link->sink->entity);
	struct v4l2_subdev *sd =
		media_entity_to_v4l2_subdev(link->source->entity);
	struct rkisp2_capture *cap = video_get_drvdata(vdev);
	const struct rkisp2_capture_fmt_cfg *fmt =
		rkisp2_find_fmt_cfg(cap, cap->pix.fmt.pixelformat);
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};
	int ret;

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret)
		return ret;

	if (sd_fmt.format.height != cap->pix.fmt.height ||
	    sd_fmt.format.width != cap->pix.fmt.width ||
	    sd_fmt.format.code != fmt->mbus) {
		dev_dbg(cap->rkisp2->dev,
			"link '%s':%u -> '%s':%u not valid: 0x%04x/%ux%u != 0x%04x/%ux%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.code, sd_fmt.format.width,
			sd_fmt.format.height, fmt->mbus, cap->pix.fmt.width,
			cap->pix.fmt.height);
		return -EPIPE;
	}

	return 0;
}

/* ----------------------------------------------------------------------------
 * core functions
 */

static const struct media_entity_operations rkisp2_media_ops = {
	.link_validate = rkisp2_capture_link_validate,
};

static const struct v4l2_file_operations rkisp2_fops = {
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static void rkisp2_unregister_capture(struct rkisp2_capture *cap)
{
	if (!video_is_registered(&cap->vnode.vdev))
		return;

	media_entity_cleanup(&cap->vnode.vdev.entity);
	vb2_video_unregister_device(&cap->vnode.vdev);
	mutex_destroy(&cap->vnode.vlock);
}

void rkisp2_capture_devs_unregister(struct rkisp2_device *rkisp2)
{
	struct rkisp2_capture *mp = &rkisp2->capture_devs[RKISP2_MAINPATH];
	struct rkisp2_capture *sp = &rkisp2->capture_devs[RKISP2_SELFPATH];

	rkisp2_unregister_capture(mp);
	rkisp2_unregister_capture(sp);
}

static int rkisp2_register_capture(struct rkisp2_capture *cap)
{
	static const char * const dev_names[] = {
		RKISP2_MP_DEV_NAME, RKISP2_SP_DEV_NAME
	};
	struct v4l2_device *v4l2_dev = &cap->rkisp2->v4l2_dev;
	struct video_device *vdev = &cap->vnode.vdev;
	struct rkisp2_vdev_node *node;
	struct vb2_queue *q;
	int ret;

	strscpy(vdev->name, dev_names[cap->id], sizeof(vdev->name));
	node = rkisp2_vdev_to_node(vdev);
	mutex_init(&node->vlock);

	vdev->ioctl_ops = &rkisp2_v4l2_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->fops = &rkisp2_fops;
	vdev->minor = -1;
	vdev->v4l2_dev = v4l2_dev;
	vdev->lock = &node->vlock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE |
			    V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
	vdev->entity.ops = &rkisp2_media_ops;
	video_set_drvdata(vdev, cap);
	vdev->vfl_dir = VFL_DIR_RX;
	node->pad.flags = MEDIA_PAD_FL_SINK;

	q = &node->buf_queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = cap;
	q->ops = &rkisp2_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->gfp_flags = GFP_DMA32;
	q->buf_struct_size = sizeof(struct rkisp2_buffer);
	q->min_queued_buffers = 1;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->vlock;
	q->dev = cap->rkisp2->dev;
	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(cap->rkisp2->dev,
			"vb2 queue init failed (err=%d)\n", ret);
		goto error;
	}

	vdev->queue = q;

	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto error;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(cap->rkisp2->dev,
			"failed to register %s, ret=%d\n", vdev->name, ret);
		goto error;
	}

	v4l2_info(v4l2_dev, "registered %s as /dev/video%d\n", vdev->name,
		  vdev->num);

	return 0;

error:
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
	return ret;
}

static void
rkisp2_capture_init(struct rkisp2_device *rkisp2, enum rkisp2_stream_id id)
{
	struct rkisp2_capture *cap = &rkisp2->capture_devs[id];
	struct v4l2_pix_format_mplane pixm;

	memset(cap, 0, sizeof(*cap));
	cap->id = id;
	cap->rkisp2 = rkisp2;

	INIT_LIST_HEAD(&cap->buf.queue);
	init_waitqueue_head(&cap->done);
	spin_lock_init(&cap->buf.lock);
	if (cap->id == RKISP2_SELFPATH) {
		cap->ops = &rkisp2_capture_ops_sp;
		cap->config = &rkisp2_capture_config_sp;
	} else {
		cap->ops = &rkisp2_capture_ops_mp;
		cap->config = &rkisp2_capture_config_mp;
	}

	cap->is_streaming = false;

	memset(&pixm, 0, sizeof(pixm));
	pixm.pixelformat = V4L2_PIX_FMT_YUYV;
	pixm.width = RKISP2_DEFAULT_WIDTH;
	pixm.height = RKISP2_DEFAULT_HEIGHT;
	rkisp2_set_fmt(cap, &pixm);
}

int rkisp2_capture_devs_register(struct rkisp2_device *rkisp2)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		struct rkisp2_capture *cap = &rkisp2->capture_devs[i];

		rkisp2_capture_init(rkisp2, i);

		ret = rkisp2_register_capture(cap);
		if (ret) {
			rkisp2_capture_devs_unregister(rkisp2);
			return ret;
		}
	}

	return 0;

}
