// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2020 Maxime Chevallier <maxime.chevallier@bootlin.com>
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-dma-contig.h>

#include "cif-capture-dvp.h"
#include "cif-common.h"
#include "cif-regs.h"
#include "cif-stream.h"

static struct cif_output_fmt dvp_out_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV16,
		.dvp_fmt_val = CIF_FORMAT_YUV_OUTPUT_422 |
			       CIF_FORMAT_UV_STORAGE_ORDER_UVUV,
		.cplanes = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV61,
		.dvp_fmt_val = CIF_FORMAT_YUV_OUTPUT_422 |
			       CIF_FORMAT_UV_STORAGE_ORDER_VUVU,
		.cplanes = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.dvp_fmt_val = CIF_FORMAT_YUV_OUTPUT_420 |
			       CIF_FORMAT_UV_STORAGE_ORDER_UVUV,
		.cplanes = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV21,
		.dvp_fmt_val = CIF_FORMAT_YUV_OUTPUT_420 |
			       CIF_FORMAT_UV_STORAGE_ORDER_VUVU,
		.cplanes = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_RGB24,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_RGB565,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_BGR666,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SRGGB8,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG8,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG8,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR8,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SRGGB10,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG10,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG10,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR10,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SRGGB12,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG12,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG12,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR12,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR16,
		.cplanes = 1,
	},
	{
		.fourcc = V4L2_PIX_FMT_Y16,
		.cplanes = 1,
	},
};

static const struct cif_input_fmt px30_dvp_in_fmts[] = {
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_UYVY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_UYVY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_VYUY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_VYUY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_RGB888_1X24,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	}
};

const struct cif_dvp_match_data px30_vip_dvp_match_data = {
	.in_fmts = px30_dvp_in_fmts,
	.in_fmts_num = ARRAY_SIZE(px30_dvp_in_fmts),
	.out_fmts = dvp_out_fmts,
	.out_fmts_num = ARRAY_SIZE(dvp_out_fmts),
	.has_scaler = true,
	.regs = {
		[CIF_DVP_CTRL] = 0x00,
		[CIF_DVP_INTEN] = 0x04,
		[CIF_DVP_INTSTAT] = 0x08,
		[CIF_DVP_FOR] = 0x0c,
		[CIF_DVP_LINE_NUM_ADDR] = 0x10,
		[CIF_DVP_FRM0_ADDR_Y] = 0x14,
		[CIF_DVP_FRM0_ADDR_UV] = 0x18,
		[CIF_DVP_FRM1_ADDR_Y] = 0x1c,
		[CIF_DVP_FRM1_ADDR_UV] = 0x20,
		[CIF_DVP_VIR_LINE_WIDTH] = 0x24,
		[CIF_DVP_SET_SIZE] = 0x28,
		[CIF_DVP_SCL_CTRL] = 0x48,
		[CIF_DVP_FRAME_STATUS] = 0x60,
		[CIF_DVP_LAST_LINE] = 0x68,
		[CIF_DVP_LAST_PIX] = 0x6c,
	},
};

static const struct cif_input_fmt rk3568_dvp_in_fmts[] = {
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_UYVY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_UYVY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_VYUY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_2X8,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_VYUY,
		.fmt_type = CIF_FMT_TYPE_YUV,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV |
			       CIF_FORMAT_INPUT_MODE_BT1120 |
			       CIF_FORMAT_BT1120_TRANSMIT_PROGRESS,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YUYV8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV |
			       CIF_FORMAT_INPUT_MODE_BT1120,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU |
			       CIF_FORMAT_INPUT_MODE_BT1120 |
			       CIF_FORMAT_BT1120_TRANSMIT_PROGRESS,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_YVYU8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU |
			       CIF_FORMAT_INPUT_MODE_BT1120,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV |
			       CIF_FORMAT_INPUT_MODE_BT1120 |
			       CIF_FORMAT_BT1120_YC_SWAP |
			       CIF_FORMAT_BT1120_TRANSMIT_PROGRESS,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YUYV |
			       CIF_FORMAT_BT1120_YC_SWAP |
			       CIF_FORMAT_INPUT_MODE_BT1120,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU |
			       CIF_FORMAT_INPUT_MODE_BT1120 |
			       CIF_FORMAT_BT1120_YC_SWAP |
			       CIF_FORMAT_BT1120_TRANSMIT_PROGRESS,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_VYUY8_1X16,
		.dvp_fmt_val = CIF_FORMAT_YUV_INPUT_422 |
			       CIF_FORMAT_YUV_INPUT_ORDER_YVYU |
			       CIF_FORMAT_BT1120_YC_SWAP |
			       CIF_FORMAT_INPUT_MODE_BT1120,
		.field = V4L2_FIELD_INTERLACED,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGBRG12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SGRBG12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_RGB888_1X24,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y8_1X8,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_8,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y10_1X10,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_10,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
	{
		.mbus_code = MEDIA_BUS_FMT_Y12_1X12,
		.dvp_fmt_val = CIF_FORMAT_INPUT_MODE_RAW |
			       CIF_FORMAT_RAW_DATA_WIDTH_12,
		.fmt_type = CIF_FMT_TYPE_RAW,
		.field = V4L2_FIELD_NONE,
	},
};

static void rk3568_dvp_grf_setup(struct cif_device *cif_dev)
{
	u32 con1 = RK3568_GRF_WRITE_ENABLE(RK3568_GRF_VI_CON1_CIF_DATAPATH |
					   RK3568_GRF_VI_CON1_CIF_CLK_DELAYNUM);

	if (!cif_dev->grf)
		return;

	con1 |= RK3568_GRF_SET_CLK_DELAYNUM(cif_dev->dvp.cif_clk_delaynum);

	if (cif_dev->dvp.vep.bus.parallel.flags &
	    V4L2_MBUS_PCLK_SAMPLE_DUALEDGE)
		con1 |= RK3568_GRF_VI_CON1_CIF_DATAPATH;

	regmap_write(cif_dev->grf, RK3568_GRF_VI_CON1, con1);
}

const struct cif_dvp_match_data rk3568_vicap_dvp_match_data = {
	.in_fmts = rk3568_dvp_in_fmts,
	.in_fmts_num = ARRAY_SIZE(rk3568_dvp_in_fmts),
	.out_fmts = dvp_out_fmts,
	.out_fmts_num = ARRAY_SIZE(dvp_out_fmts),
	.setup = rk3568_dvp_grf_setup,
	.has_scaler = false,
	.regs = {
		[CIF_DVP_CTRL] = 0x00,
		[CIF_DVP_INTEN] = 0x04,
		[CIF_DVP_INTSTAT] = 0x08,
		[CIF_DVP_FOR] = 0x0c,
		[CIF_DVP_LINE_NUM_ADDR] = 0x2c,
		[CIF_DVP_FRM0_ADDR_Y] = 0x14,
		[CIF_DVP_FRM0_ADDR_UV] = 0x18,
		[CIF_DVP_FRM1_ADDR_Y] = 0x1c,
		[CIF_DVP_FRM1_ADDR_UV] = 0x20,
		[CIF_DVP_VIR_LINE_WIDTH] = 0x24,
		[CIF_DVP_SET_SIZE] = 0x28,
		[CIF_DVP_FRAME_STATUS] = 0x3c,
		[CIF_DVP_LAST_LINE] = 0x44,
		[CIF_DVP_LAST_PIX] = 0x48,
	},
};

static inline unsigned int cif_dvp_get_addr(struct cif_device *cif_device,
					    unsigned int index)
{
	if (index >= CIF_DVP_REGISTER_MAX)
		return CIF_DVP_REGISTER_INVALID;

	return cif_device->match_data->dvp->regs[index];
}

static inline void cif_dvp_write(struct cif_device *cif_dev, unsigned int index,
				 u32 val)
{
	unsigned int addr = cif_dvp_get_addr(cif_dev, index);

	if (addr == CIF_DVP_REGISTER_INVALID)
		return;

	writel(val, cif_dev->base_addr + addr);
}

static inline u32 cif_dvp_read(struct cif_device *cif_dev, unsigned int index)
{
	unsigned int addr = cif_dvp_get_addr(cif_dev, index);

	if (addr == CIF_DVP_REGISTER_INVALID)
		return 0;

	return readl(cif_dev->base_addr + addr);
}

static void cif_dvp_queue_buffer(struct cif_stream *stream, unsigned int index)
{
	struct cif_device *cif_dev = stream->cif_dev;
	struct cif_buffer *buffer = stream->buffers[index];
	u32 frm_addr_y, frm_addr_uv;

	frm_addr_y = index ? CIF_DVP_FRM1_ADDR_Y : CIF_DVP_FRM0_ADDR_Y;
	frm_addr_uv = index ? CIF_DVP_FRM1_ADDR_UV : CIF_DVP_FRM0_ADDR_UV;

	cif_dvp_write(cif_dev, frm_addr_y, buffer->buff_addr[CIF_PLANE_Y]);
	cif_dvp_write(cif_dev, frm_addr_uv, buffer->buff_addr[CIF_PLANE_UV]);
}

static int cif_dvp_start_streaming(struct cif_stream *stream)
{
	u32 val, fmt_type, xfer_mode = 0;
	struct cif_device *cif_dev = stream->cif_dev;
	struct v4l2_mbus_config_parallel *parallel;

	fmt_type = stream->active_in_fmt->fmt_type;

	parallel = &cif_dev->dvp.vep.bus.parallel;
	if ((parallel->bus_width == 16) &&
	    (parallel->flags & V4L2_MBUS_PCLK_SAMPLE_DUALEDGE))
		xfer_mode |= CIF_FORMAT_BT1120_CLOCK_DOUBLE_EDGES;

	val = stream->active_out_fmt->dvp_fmt_val |
	      stream->active_in_fmt->dvp_fmt_val | xfer_mode;
	cif_dvp_write(cif_dev, CIF_DVP_FOR, val);

	val = stream->pix.width;
	if (stream->active_in_fmt->fmt_type == CIF_FMT_TYPE_RAW)
		val = stream->pix.width * 2;

	cif_dvp_write(cif_dev, CIF_DVP_VIR_LINE_WIDTH, val);
	cif_dvp_write(cif_dev, CIF_DVP_SET_SIZE,
		      stream->pix.width | (stream->pix.height << 16));

	cif_dvp_write(cif_dev, CIF_DVP_FRAME_STATUS, CIF_FRAME_STAT_CLS);
	cif_dvp_write(cif_dev, CIF_DVP_INTSTAT, CIF_INTSTAT_CLS);
	if (cif_dev->match_data->dvp->has_scaler)
		cif_dvp_write(cif_dev, CIF_DVP_SCL_CTRL,
			      (fmt_type == CIF_FMT_TYPE_YUV) ?
				      CIF_SCL_CTRL_ENABLE_YUV_16BIT_BYPASS :
				      CIF_SCL_CTRL_ENABLE_RAW_16BIT_BYPASS);

	cif_dvp_write(cif_dev, CIF_DVP_INTEN,
		      CIF_INTEN_FRAME_END_EN | CIF_INTEN_PST_INF_FRAME_END_EN);

	cif_dvp_write(cif_dev, CIF_DVP_CTRL,
		      CIF_CTRL_AXI_BURST_16 | CIF_CTRL_MODE_PINGPONG |
			      CIF_CTRL_ENABLE_CAPTURE);
	return 0;
}

static void cif_dvp_stop_streaming(struct cif_stream *stream)
{
	struct cif_device *cif_dev = stream->cif_dev;
	u32 val;

	val = cif_dvp_read(cif_dev, CIF_DVP_CTRL);
	cif_dvp_write(cif_dev, CIF_DVP_CTRL, val & (~CIF_CTRL_ENABLE_CAPTURE));
	cif_dvp_write(cif_dev, CIF_DVP_INTEN, 0x0);
	cif_dvp_write(cif_dev, CIF_DVP_INTSTAT, 0x3ff);
	cif_dvp_write(cif_dev, CIF_DVP_FRAME_STATUS, 0x0);

	stream->stopping = false;
}

static void cif_dvp_reset_stream(struct cif_device *cif_dev)
{
	u32 ctl = cif_dvp_read(cif_dev, CIF_DVP_CTRL);

	cif_dvp_write(cif_dev, CIF_DVP_CTRL, ctl & (~CIF_CTRL_ENABLE_CAPTURE));
	cif_dvp_write(cif_dev, CIF_DVP_CTRL, ctl | CIF_CTRL_ENABLE_CAPTURE);
}

irqreturn_t cif_dvp_isr(int irq, void *ctx)
{
	struct device *dev = ctx;
	struct cif_device *cif_dev = dev_get_drvdata(dev);
	struct cif_stream *stream = &cif_dev->dvp.stream;
	unsigned int intstat;
	u32 lastline, lastpix, ctl, cif_frmst;
	irqreturn_t ret = IRQ_NONE;

	if (!cif_dev->match_data->dvp)
		return ret;

	intstat = cif_dvp_read(cif_dev, CIF_DVP_INTSTAT);
	cif_frmst = cif_dvp_read(cif_dev, CIF_DVP_FRAME_STATUS);
	lastline =
		CIF_FETCH_Y_LAST_LINE(cif_dvp_read(cif_dev, CIF_DVP_LAST_LINE));
	lastpix =
		CIF_FETCH_Y_LAST_LINE(cif_dvp_read(cif_dev, CIF_DVP_LAST_PIX));
	ctl = cif_dvp_read(cif_dev, CIF_DVP_CTRL);

	/*
	 * The following IRQs are enabled:
	 *  - PST_INF_FRAME_END: cif FIFO is ready, this is prior to FRAME_END
	 *  - FRAME_END: cif has saved frame to memory
	 */

	if (intstat & CIF_INTSTAT_PST_INF_FRAME_END) {
		cif_dvp_write(cif_dev, CIF_DVP_INTSTAT,
			      CIF_INTSTAT_PST_INF_FRAME_END_CLR);

		if (stream->stopping)
			/* To stop cif ASAP, before FRAME_END IRQ */
			cif_dvp_write(cif_dev, CIF_DVP_CTRL,
				      ctl & (~CIF_CTRL_ENABLE_CAPTURE));

		ret = IRQ_HANDLED;
	}

	if (intstat & CIF_INTSTAT_FRAME_END) {
		cif_dvp_write(cif_dev, CIF_DVP_INTSTAT,
			      CIF_INTSTAT_FRAME_END_CLR |
				      CIF_INTSTAT_LINE_END_CLR);

		if (stream->stopping) {
			cif_dvp_stop_streaming(stream);
			wake_up(&stream->wq_stopped);
			return IRQ_HANDLED;
		}

		if (lastline != stream->pix.height) {
			v4l2_err(&cif_dev->v4l2_dev,
				 "bad frame, irq:%#x frmst:%#x size:%dx%d\n",
				 intstat, cif_frmst, lastpix, lastline);

			cif_dvp_reset_stream(cif_dev);
		}

		cif_stream_pingpong(stream);

		ret = IRQ_HANDLED;
	}

	return ret;
}

int cif_dvp_register(struct cif_device *cif_dev)
{
	struct cif_stream *stream = &cif_dev->dvp.stream;
	const struct cif_stream_config config = {
		.name = CIF_DRIVER_NAME "-dvp",
	};
	int ret;

	stream->in_fmts = cif_dev->match_data->dvp->in_fmts;
	stream->in_fmts_num = cif_dev->match_data->dvp->in_fmts_num;
	stream->out_fmts = cif_dev->match_data->dvp->out_fmts;
	stream->out_fmts_num = cif_dev->match_data->dvp->out_fmts_num;
	stream->queue_buffer = cif_dvp_queue_buffer;
	stream->start_streaming = cif_dvp_start_streaming;
	stream->stop_streaming = cif_dvp_stop_streaming;

	if (cif_dev->match_data->dvp->setup)
		cif_dev->match_data->dvp->setup(cif_dev);

	ret = cif_stream_register(cif_dev, stream, &config);
	if (ret)
		return ret;

	return 0;
}

void cif_dvp_unregister(struct cif_device *cif_dev)
{
	struct cif_stream *stream = &cif_dev->dvp.stream;

	cif_stream_unregister(stream);
}
