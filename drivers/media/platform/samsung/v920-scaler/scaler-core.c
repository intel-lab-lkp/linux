// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd. All Rights Reserved
 *		http://www.samsung.com
 *
 * Core file for Samsung EXYNOS Scaler driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/version.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/pm_runtime.h>
#include <linux/iommu.h>
#include <linux/iosys-map.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>

#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-sg.h>

#include "scaler.h"
#include "scaler-regs.h"

/* Protection IDs of Scaler are 2 and 3. */
int sc_show_stat;
module_param_named(sc_show_stat, sc_show_stat, uint, 0644);

#define BUF_EXT_SIZE	512
#define BUF_WIDTH_ALIGN	128

static inline unsigned int sc_ext_buf_size(int width)
{
	return IS_ALIGNED(width, BUF_WIDTH_ALIGN) ? 0 : BUF_EXT_SIZE;
}

struct vb2_sc_buffer {
	struct v4l2_m2m_buffer mb;
	ktime_t ktime;

	struct dma_fence	*in_fence;
	int			state;
	struct dma_fence_cb	fence_cb;
	struct timer_list	fence_timer;
	struct work_struct	qbuf_work;

	struct sync_file	*out_sync_file;
};

static const struct sc_fmt sc_formats[] = {
	{
		.name		= "RGB565",
		.pixelformat	= V4L2_PIX_FMT_RGB565,
		.cfg_val	= SCALER_CFG_FMT_RGB565,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "RGB1555",
		.pixelformat	= V4L2_PIX_FMT_RGB555X,
		.cfg_val	= SCALER_CFG_FMT_ARGB1555,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "ARGB4444",
		.pixelformat	= V4L2_PIX_FMT_RGB444,
		.cfg_val	= SCALER_CFG_FMT_ARGB4444,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {	/* swaps of ARGB32 in bytes in half word, half words in word */
		.name		= "RGBA8888",
		.pixelformat	= V4L2_PIX_FMT_RGB32,
		.cfg_val	= SCALER_CFG_FMT_RGBA8888 |
					SCALER_CFG_BYTE_HWORD_SWAP,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "BGRA8888",
		.pixelformat	= V4L2_PIX_FMT_BGR32,
		.cfg_val	= SCALER_CFG_FMT_ARGB8888,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "ARGB2101010",
		.pixelformat	= V4L2_PIX_FMT_ARGB2101010,
		.cfg_val	= SCALER_CFG_FMT_ARGB2101010,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "ABGR2101010",
		.pixelformat	= V4L2_PIX_FMT_ABGR2101010,
		.cfg_val	= SCALER_CFG_FMT_ABGR2101010,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "RGBA1010102",
		.pixelformat	= V4L2_PIX_FMT_RGBA1010102,
		.cfg_val	= SCALER_CFG_FMT_RGBA1010102,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "BGRA1010102",
		.pixelformat	= V4L2_PIX_FMT_BGRA1010102,
		.cfg_val	= SCALER_CFG_FMT_BGRA1010102,
		.bitperpixel	= { 32 },
		.num_planes	= 1,
		.num_comp	= 1,
		.is_rgb		= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV21,
		.cfg_val	= SCALER_CFG_FMT_YCRCB420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV21M,
		.cfg_val	= SCALER_CFG_FMT_YCRCB420_2P,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 2-planar, Y/CbCr, tiled",
		.pixelformat	= V4L2_PIX_FMT_NV12MT_16X16,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_TILE_EN,
		.bitperpixel	= { 8, 4 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420,	/* I420 */
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YVU 4:2:0 contiguous 3-planar, Y/Cr/Cb",
		.pixelformat	= V4L2_PIX_FMT_YVU420,	/* YV12 */
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 non-contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 8, 2, 2 },
		.num_planes	= 3,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YVU 4:2:0 non-contiguous 3-planar, Y/Cr/Cb",
		.pixelformat	= V4L2_PIX_FMT_YVU420M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 8, 2, 2 },
		.num_planes	= 3,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, YCbYCr",
		.pixelformat	= V4L2_PIX_FMT_YUYV,
		.cfg_val	= SCALER_CFG_FMT_YUYV,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, CbYCrY",
		.pixelformat	= V4L2_PIX_FMT_UYVY,
		.cfg_val	= SCALER_CFG_FMT_UYVY,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 packed, YCrYCb",
		.pixelformat	= V4L2_PIX_FMT_YVYU,
		.cfg_val	= SCALER_CFG_FMT_YVYU,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 1,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV16,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_2P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV61,
		.cfg_val	= SCALER_CFG_FMT_YCRCB422_2P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 not-contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV16M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_2P,
		.bitperpixel	= { 8, 8 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 not-contiguous 2-planar, Y/CrCb",
		.pixelformat	= V4L2_PIX_FMT_NV61M,
		.cfg_val	= SCALER_CFG_FMT_YCRCB422_2P,
		.bitperpixel	= { 8, 8 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV422P,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_3P,
		.bitperpixel	= { 16 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 not-contiguous 3-planar, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV422M,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_3P,
		.bitperpixel	= { 8, 4, 4 },
		.num_planes	= 3,
		.num_comp	= 3,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12N,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous Y/CbCr 10-bit",
		.pixelformat	= V4L2_PIX_FMT_NV12N_10B,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_10BIT_S10,
		.bitperpixel	= { 15 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 3-planar Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420N,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_3P,
		.bitperpixel	= { 12 },
		.num_planes	= 1,
		.num_comp	= 3,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CbCr 8+2 bit",
		.pixelformat	= V4L2_PIX_FMT_NV12M_S10B,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_10BIT_S10,
		.bitperpixel	= { 10, 5 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous 2-planar, Y/CbCr 10-bit",
		.pixelformat	= V4L2_PIX_FMT_NV12M_P010,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_BYTE_SWAP |
					SCALER_CFG_10BIT_P010,
		.bitperpixel	= { 16, 8 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:0 contiguous, Y/CbCr 10-bit",
		.pixelformat	= V4L2_PIX_FMT_NV12_P010,
		.cfg_val	= SCALER_CFG_FMT_YCBCR420_2P |
					SCALER_CFG_BYTE_SWAP |
					SCALER_CFG_10BIT_P010,
		.bitperpixel	= { 24 },
		.num_planes	= 1,
		.num_comp	= 2,
		.h_shift	= 1,
		.v_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CbCr 8+2 bit",
		.pixelformat	= V4L2_PIX_FMT_NV16M_S10B,
		.cfg_val	= SCALER_CFG_FMT_YCBCR422_2P |
					SCALER_CFG_10BIT_S10,
		.bitperpixel	= { 10, 10 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
	}, {
		.name		= "YUV 4:2:2 contiguous 2-planar, Y/CrCb 8+2 bit",
		.pixelformat	= V4L2_PIX_FMT_NV61M_S10B,
		.cfg_val	= SCALER_CFG_FMT_YCRCB422_2P |
					SCALER_CFG_10BIT_S10,
		.bitperpixel	= { 10, 10 },
		.num_planes	= 2,
		.num_comp	= 2,
		.h_shift	= 1,
	},
};

#define SCALE_RATIO_CONST(x, y) ((u32)((1048576ULL * (x)) / (y)))

#define SCALE_RATIO(x, y)							\
({										\
		u32 ratio;							\
		typeof(x) _x = (x);						\
		typeof(y) _y = (y);						\
		if (__builtin_constant_p(_x) && __builtin_constant_p(_y)) {	\
			ratio = SCALE_RATIO_CONST(_x, _y);			\
		} else if ((_x) < 2048) {					\
			ratio = (u32)((1048576UL * (_x)) / (_y));		\
		} else {							\
			unsigned long long dividend = 1048576ULL;		\
			dividend *= _x;						\
			do_div(dividend, _y);					\
			ratio = (u32)dividend;					\
		}								\
		ratio;								\
})

#define SCALE_RATIO_FRACT(x, y, z) ((u32)((((x) << 20) + SCALER_FRACT_VAL(y)) / (z)))

static const u32 sc_version_table[][2] = {
	{ 0x07000102, SCALER_VERSION(7, 0, 1) }, /* SC_POLY */
	{ 0x04000002, SCALER_VERSION(5, 1, 0) }, /* SC_POLY */
	{ 0x04000001, SCALER_VERSION(5, 1, 0) }, /* SC_POLY */
	{ 0x04000000, SCALER_VERSION(5, 1, 0) }, /* SC_POLY */
	{ 0x02000100, SCALER_VERSION(5, 0, 1) }, /* SC_POLY */
	{ 0x02000000, SCALER_VERSION(5, 0, 0) },
	{ 0x80060007, SCALER_VERSION(4, 2, 0) }, /* SC_BI */
	{ 0x0100000f, SCALER_VERSION(4, 0, 1) }, /* SC_POLY */
	{ 0xA0000013, SCALER_VERSION(4, 0, 1) },
	{ 0xA0000012, SCALER_VERSION(4, 0, 1) },
	{ 0x80050007, SCALER_VERSION(4, 0, 0) }, /* SC_POLY */
	{ 0xA000000B, SCALER_VERSION(3, 0, 2) },
	{ 0xA000000A, SCALER_VERSION(3, 0, 2) },
	{ 0x8000006D, SCALER_VERSION(3, 0, 1) },
	{ 0x80000068, SCALER_VERSION(3, 0, 0) },
	{ 0x8004000C, SCALER_VERSION(2, 2, 0) },
	{ 0x80000008, SCALER_VERSION(2, 1, 1) },
	{ 0x80000048, SCALER_VERSION(2, 1, 0) },
	{ 0x80010000, SCALER_VERSION(2, 0, 1) },
	{ 0x80000047, SCALER_VERSION(2, 0, 0) },
};

static const struct sc_variant sc_variant[] = {
	{
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 16384,
			.max_h		= 16384,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 16384,
			.max_h		= 16384,
		},
		.version		= SCALER_VERSION(7, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 64),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.ratio_20bit		= 1,
		.initphase		= 1,
		.pixfmt_10bit		= 1,
		.color_fill		= 1,

	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(5, 1, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 64),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
		.pixfmt_10bit		= 1,
		.color_fill		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(5, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 64),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
		.pixfmt_10bit		= 1,
		.extra_buf		= 1,
		.color_fill		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(5, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 64),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
		.pixfmt_10bit		= 1,
		.extra_buf		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 2, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 1,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_up_swmax		= SCALE_RATIO_CONST(1, 16),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(4, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(3, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(16, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 1,
		.ratio_20bit		= 1,
		.initphase		= 1,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(2, 2, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 1,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.version		= SCALER_VERSION(2, 0, 1),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	}, {
		.limit_input = {
			.min_w		= 16,
			.min_h		= 16,
			.max_w		= 8192,
			.max_h		= 8192,
		},
		.limit_output = {
			.min_w		= 4,
			.min_h		= 4,
			.max_w		= 4096,
			.max_h		= 4096,
		},
		.version		= SCALER_VERSION(2, 0, 0),
		.sc_up_max		= SCALE_RATIO_CONST(1, 8),
		.sc_down_min		= SCALE_RATIO_CONST(4, 1),
		.sc_down_swmin		= SCALE_RATIO_CONST(16, 1),
		.blending		= 0,
		.prescale		= 0,
		.ratio_20bit		= 0,
		.initphase		= 0,
	},
};

static const struct sc_fmt *sc_find_format(struct sc_dev *sc,
					   u32 pixfmt, bool output_buf)
{
	const struct sc_fmt *sc_fmt;
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(sc_formats); ++i) {
		sc_fmt = &sc_formats[i];
		if (sc_fmt->pixelformat == pixfmt) {
			if (!!(sc_fmt->cfg_val & SCALER_CFG_TILE_EN)) {
				/* tile mode is not supported from v3.0.0 */
				if (sc->version >= SCALER_VERSION(3, 0, 0))
					return NULL;
				if (!output_buf)
					return NULL;
			}
			/* bytes swap is not supported under v2.1.0 */
			if (!!(sc_fmt->cfg_val & SCALER_CFG_SWAP_MASK) &&
			    sc->version < SCALER_VERSION(2, 1, 0))
				return NULL;
			return &sc_formats[i];
		}
	}

	return NULL;
}

static int sc_v4l2_querycap(struct file *file, void *fh,
			    struct v4l2_capability *cap)
{
	strscpy(cap->driver, MODULE_NAME, strlen(MODULE_NAME) + 1);
	strscpy(cap->card, MODULE_NAME, strlen(MODULE_NAME) + 1);

	return 0;
}

static int sc_v4l2_g_fmt_mplane(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	const struct sc_fmt *sc_fmt;
	struct sc_frame *frame;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	int i;

	frame = ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	sc_fmt = frame->sc_fmt;

	pixm->width		= frame->width;
	pixm->height		= frame->height;
	pixm->pixelformat	= frame->pixelformat;
	pixm->field		= V4L2_FIELD_NONE;
	pixm->num_planes	= frame->sc_fmt->num_planes;
	pixm->colorspace	= 0;

	for (i = 0; i < pixm->num_planes; ++i) {
		pixm->plane_fmt[i].bytesperline = (pixm->width *
				sc_fmt->bitperpixel[i]) >> 3;
		if (sc_fmt_is_ayv12(sc_fmt->pixelformat)) {
			unsigned int y_size, c_span;

			y_size = pixm->width * pixm->height;
			c_span = ALIGN(pixm->width >> 1, 16);
			pixm->plane_fmt[i].sizeimage =
				y_size + (c_span * pixm->height >> 1) * 2;
		} else {
			pixm->plane_fmt[i].sizeimage =
				pixm->plane_fmt[i].bytesperline * pixm->height;
		}
	}

	return 0;
}

int sc_calc_s10b_planesize(u32 pixelformat, u32 width, u32 height,
			   u32 *ysize, u32 *csize, bool only_8bit, u8 byte32num)
{
	int ret = 0;
	int c_padding = 0;

	switch (pixelformat) {
	case V4L2_PIX_FMT_NV12M_S10B:
	case V4L2_PIX_FMT_NV12N_10B:
			*ysize = NV12M_Y_SIZE(width, height);
			*csize = NV12M_CBCR_SIZE(width, height);
		break;
	case V4L2_PIX_FMT_NV16M_S10B:
	case V4L2_PIX_FMT_NV61M_S10B:
			*ysize = NV16M_Y_SIZE(width, height);
			*csize = NV16M_CBCR_SIZE(width, height);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret || only_8bit)
		return ret;

	switch (pixelformat) {
	case V4L2_PIX_FMT_NV12M_S10B:
	case V4L2_PIX_FMT_NV12N_10B:
			*ysize += NV12M_Y_2B_SIZE(width, height);
			*csize += NV12M_CBCR_2B_SIZE(width, height);
			c_padding = 256;
		break;
	case V4L2_PIX_FMT_NV16M_S10B:
	case V4L2_PIX_FMT_NV61M_S10B:
			*ysize += NV16M_Y_2B_SIZE(width, height);
			*csize += NV16M_CBCR_2B_SIZE(width, height);
			c_padding = 256;
		break;
	}

	*csize -= c_padding;

	return ret;
}

static int sc_v4l2_try_fmt_mplane(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	const struct sc_fmt *sc_fmt;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	const struct sc_size_limit *limit;
	int i;
	int h_align = 0;
	int w_align = 0;
	unsigned int ext_size = 0;
	struct sc_frame *frame;

	if (!V4L2_TYPE_IS_MULTIPLANAR(f->type)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "not supported v4l2 type\n");
		return -EINVAL;
	}

	sc_fmt = sc_find_format(ctx->sc_dev, f->fmt.pix_mp.pixelformat,
				V4L2_TYPE_IS_OUTPUT(f->type));
	if (!sc_fmt) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "not supported format type\n");
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		limit = &ctx->sc_dev->variant->limit_input;
	else
		limit = &ctx->sc_dev->variant->limit_output;

	/*
	 * Y_SPAN - should even in interleaved YCbCr422
	 * C_SPAN - should even in YCbCr420 and YCbCr422
	 */
	w_align = sc_fmt->h_shift;
	h_align = sc_fmt->v_shift;

	/* Bound an image to have width and height in limit */
	v4l_bound_align_image(&pixm->width, limit->min_w, limit->max_w,
			      w_align, &pixm->height, limit->min_h,
			      limit->max_h, h_align, 0);

	pixm->num_planes = sc_fmt->num_planes;
	pixm->colorspace = 0;

	if (ctx->sc_dev->variant->extra_buf && V4L2_TYPE_IS_OUTPUT(f->type))
		ext_size = sc_ext_buf_size(pixm->width);

	frame = ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	for (i = 0; i < pixm->num_planes; ++i) {
		pixm->plane_fmt[i].bytesperline = (pixm->width *
				sc_fmt->bitperpixel[i]) >> 3;
		if (sc_fmt_is_ayv12(sc_fmt->pixelformat)) {
			unsigned int y_size, c_span;

			y_size = pixm->width * pixm->height;
			c_span = ALIGN(pixm->width >> 1, 16);
			pixm->plane_fmt[i].sizeimage =
				y_size + (c_span * pixm->height >> 1) * 2;
		} else {
			pixm->plane_fmt[i].sizeimage =
				pixm->plane_fmt[i].bytesperline * pixm->height;
		}
	}

	for (i = 0; ext_size && i < pixm->num_planes; i++)
		pixm->plane_fmt[i].sizeimage += (i == 0) ? ext_size : ext_size / 2;

	return 0;
}

static int sc_v4l2_s_fmt_mplane(struct file *file, void *fh,
				struct v4l2_format *f)

{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
	struct sc_frame *frame;
	struct v4l2_pix_format_mplane *pixm = &f->fmt.pix_mp;
	const struct sc_size_limit *limitout =
				&ctx->sc_dev->variant->limit_input;
	const struct sc_size_limit *limitcap =
				&ctx->sc_dev->variant->limit_output;
	int i, ret = 0;

	if (IS_ERR_OR_NULL(vq)) {
		pr_err("[%s] vq(%p) is wrong\n", __func__, vq);
		return -EINVAL;
	}

	if (vb2_is_streaming(vq)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev, "device is busy\n");
		return -EBUSY;
	}

	ret = sc_v4l2_try_fmt_mplane(file, fh, f);
	if (ret < 0)
		return ret;

	frame = ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	set_bit(CTX_PARAMS, &ctx->flags);

	frame->sc_fmt = sc_find_format(ctx->sc_dev, f->fmt.pix_mp.pixelformat,
				       V4L2_TYPE_IS_OUTPUT(f->type));
	if (!frame->sc_fmt) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "not supported format values\n");
		return -EINVAL;
	}

	for (i = 0; i < frame->sc_fmt->num_planes; i++)
		frame->bytesused[i] = pixm->plane_fmt[i].sizeimage;

	if (V4L2_TYPE_IS_OUTPUT(f->type) &&
	    (pixm->width > limitout->max_w ||
			 pixm->height > limitout->max_h)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "%dx%d of source image is not supported: too large\n",
			pixm->width, pixm->height);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(f->type) &&
	    (pixm->width > limitcap->max_w ||
	    pixm->height > limitcap->max_h)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "%dx%d of target image is not supported: too large\n",
			 pixm->width, pixm->height);
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(f->type) &&
	    (pixm->width < limitout->min_w ||
	    pixm->height < limitout->min_h)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "%dx%d of source image is not supported: too small\n",
			 pixm->width, pixm->height);
		return -EINVAL;
	}

	if (!V4L2_TYPE_IS_OUTPUT(f->type) &&
	    (pixm->width < limitcap->min_w ||
	    pixm->height < limitcap->min_h)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "%dx%d of target image is not supported: too small\n",
			 pixm->width, pixm->height);
		return -EINVAL;
	}

	if (pixm->flags == V4L2_PIX_FMT_FLAG_PREMUL_ALPHA &&
	    ctx->sc_dev->version != SCALER_VERSION(4, 0, 0))
		frame->pre_multi = true;
	else
		frame->pre_multi = false;

	frame->width = pixm->width;
	frame->height = pixm->height;
	frame->pixelformat = pixm->pixelformat;

	frame->crop.width = pixm->width;
	frame->crop.height = pixm->height;

	return 0;
}

static int sc_v4l2_reqbufs(struct file *file, void *fh,
			   struct v4l2_requestbuffers *reqbufs)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	return v4l2_m2m_reqbufs(file, ctx->m2m_ctx, reqbufs);
}

static int sc_v4l2_querybuf(struct file *file, void *fh,
			    struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	return v4l2_m2m_querybuf(file, ctx->m2m_ctx, buf);
}

#define sc_from_vb2_to_sc_buf(vb2_buf)					       \
({									       \
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);	       \
	struct v4l2_m2m_buffer *m2m_buf =				       \
				container_of(v4l2_buf, typeof(*m2m_buf), vb);  \
	struct vb2_sc_buffer *sc_buf =					       \
				container_of(m2m_buf, typeof(*sc_buf), mb);    \
									       \
	sc_buf;								       \
})

static void __sc_vb2_buf_queue(struct v4l2_m2m_ctx *m2m_ctx,
			       struct vb2_v4l2_buffer *v4l2_buf);

static void sc_fence_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct vb2_sc_buffer *sc_buf = container_of(cb, typeof(*sc_buf),
						    fence_cb);
	struct vb2_buffer *vb = &sc_buf->mb.vb.vb2_buf;
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct dma_fence *fence;

	do {
		fence = sc_buf->in_fence;
	} while (cmpxchg(&sc_buf->in_fence, fence, NULL) != fence);

	if (!fence)
		return;

	if (fence->error) {
		dev_err(ctx->sc_dev->dev,
			"%s: in-fence: %s #%llu, error: %d\n",
		       __func__, fence->ops->get_driver_name(fence),
		       fence->seqno, fence->error);

		sc_buf->state = fence->error;
	}
	dma_fence_put(fence);

	schedule_work(&sc_buf->qbuf_work);
}

static void __sc_qbuf_work(struct work_struct *work)
{
	struct vb2_sc_buffer *sc_buf = container_of(work, typeof(*sc_buf),
						    qbuf_work);
	struct vb2_v4l2_buffer *v4l2_buf = &sc_buf->mb.vb;
	struct sc_ctx *ctx = vb2_get_drv_priv(v4l2_buf->vb2_buf.vb2_queue);

	__sc_vb2_buf_queue(ctx->m2m_ctx, v4l2_buf);
}

#define SC_FENCE_TIMEOUT	(1000)
static void sc_fence_timeout_handler(struct timer_list *t)
{
	struct vb2_sc_buffer *sc_buf = container_of(t, typeof(*sc_buf),
						    fence_timer);
	struct vb2_buffer *vb = &sc_buf->mb.vb.vb2_buf;
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct dma_fence *fence;

	do {
		fence = sc_buf->in_fence;
	} while (cmpxchg(&sc_buf->in_fence, fence, NULL) != fence);

	if (!fence)
		return;

	dma_fence_remove_callback(fence, &sc_buf->fence_cb);

	dev_err(ctx->sc_dev->dev,
		"%s: timeout(%d ms) in-fence: %s #%llu (%s), error: %d\n",
		__func__, SC_FENCE_TIMEOUT,
		fence->ops->get_driver_name(fence), fence->seqno,
		dma_fence_is_signaled(fence) ? "signaled" : "active",
		fence->error);

	sc_buf->state = -ETIMEDOUT;

	dma_fence_put(fence);

	fence = sc_buf->out_sync_file->fence;
	if (fence)
		dev_err(ctx->sc_dev->dev,
			"%s: out-fence: #%llu\n", __func__, fence->seqno);

	__sc_vb2_buf_queue(ctx->m2m_ctx, &sc_buf->mb.vb);
}

static void sc_remove_out_fence(struct vb2_sc_buffer *sc_buf)
{
	fput(sc_buf->out_sync_file->file);
	sc_buf->out_sync_file = NULL;
}

static struct vb2_sc_buffer *sc_get_sc_buf_from_v4l2_buf(struct sc_ctx *ctx,
							 struct v4l2_buffer *buf)
{
	struct vb2_queue *vq;
	struct vb2_buffer *vb;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, buf->type);
	if (IS_ERR_OR_NULL(vq))
		return ERR_PTR(-EINVAL);

	if (buf->index >= VB2_MAX_FRAME) {
		dev_err(ctx->sc_dev->dev,
			"%s: buf->index(%d) >= VB2_MAX_FRAME(%d)\n",
			__func__, buf->index, VB2_MAX_FRAME);
		return ERR_PTR(-EINVAL);
	}

	vb = vq->bufs[buf->index];
	if (IS_ERR_OR_NULL(vb)) {
		dev_err(ctx->sc_dev->dev,
			"%s: vb2_buffer is NULL\n", __func__);
		return ERR_PTR(-EFAULT);
	}

	if (IS_ERR_OR_NULL(buf->m.planes)) {
		dev_err(ctx->sc_dev->dev, "the array of planes is invalid\n");
		return ERR_PTR(-EFAULT);
	}

	if (buf->length < vb->num_planes || buf->length > VB2_MAX_PLANES) {
		dev_err(ctx->sc_dev->dev,
			"%s: buf->length is expected %d, but got %d.\n",
			__func__, buf->length, vb->num_planes);
		return ERR_PTR(-EINVAL);
	}

	return sc_from_vb2_to_sc_buf(vb);
}

static int sc_v4l2_qbuf(struct file *file, void *fh,
			struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct vb2_sc_buffer *sc_buf;
	int out_fence_fd = -1;
	int ret;

	if (IS_ERR_OR_NULL(buf)) {
		dev_err(ctx->sc_dev->dev, "%s : buf(%p) is wrong\n",
			__func__, buf);
		return -EINVAL;
	}

	sc_buf = sc_get_sc_buf_from_v4l2_buf(ctx, buf);
	if (IS_ERR(sc_buf)) {
		dev_err(ctx->sc_dev->dev, "%s : failed to get sc_buf from v4l2_buf\n",
			__func__);
		return PTR_ERR(sc_buf);
	}

	sc_buf->state = 0;

	ret = v4l2_m2m_qbuf(file, ctx->m2m_ctx, buf);
	if (ret)
		goto err;

	if (sc_buf->out_sync_file) {
		fd_install((unsigned int)out_fence_fd, get_file(sc_buf->out_sync_file->file));
		buf->reserved = (unsigned int)out_fence_fd;
	}

	return ret;

err:
	if (sc_buf->in_fence) {
		dma_fence_put(sc_buf->in_fence);
		sc_buf->in_fence = NULL;
	}
	if (sc_buf->out_sync_file)
		sc_remove_out_fence(sc_buf);
	if (out_fence_fd >= 0)
		put_unused_fd(out_fence_fd);

	return ret;
}

static int sc_v4l2_dqbuf(struct file *file, void *fh,
			 struct v4l2_buffer *buf)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	return v4l2_m2m_dqbuf(file, ctx->m2m_ctx, buf);
}

static int sc_v4l2_streamon(struct file *file, void *fh,
			    enum v4l2_buf_type type)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	return v4l2_m2m_streamon(file, ctx->m2m_ctx, type);
}

static int sc_v4l2_streamoff(struct file *file, void *fh,
			     enum v4l2_buf_type type)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);

	return v4l2_m2m_streamoff(file, ctx->m2m_ctx, type);
}

static int sc_v4l2_g_selection(struct file *file, void *fh,
			       struct v4l2_selection *s)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct sc_frame *frame;

	if (IS_ERR_OR_NULL(s)) {
		pr_err("[%s] s(%p) is wrong\n", __func__, s);
		return PTR_ERR(s);
	}

	frame = ctx_get_frame(ctx, s->type);

	if (IS_ERR(frame))
		return PTR_ERR(frame);

	s->r.left = SC_CROP_MAKE_FR_VAL(frame->crop.left, ctx->init_phase.yh);
	s->r.top = SC_CROP_MAKE_FR_VAL(frame->crop.top, ctx->init_phase.yv);
	s->r.width = SC_CROP_MAKE_FR_VAL(frame->crop.width, ctx->init_phase.w);
	s->r.height = SC_CROP_MAKE_FR_VAL(frame->crop.height, ctx->init_phase.h);

	return 0;
}

static int sc_get_fract_val(struct v4l2_rect *rect, struct sc_ctx *ctx)
{
	if (IS_ERR_OR_NULL(rect) || IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] rect(%p) or ctx(%p) is wrong\n", __func__, rect, ctx);
		return -EINVAL;
	}

	ctx->init_phase.yh = SC_CROP_GET_FR_VAL(rect->left);
	if (ctx->init_phase.yh)
		rect->left &= SC_CROP_INT_MASK;

	ctx->init_phase.yv = SC_CROP_GET_FR_VAL(rect->top);
	if (ctx->init_phase.yv)
		rect->top &= SC_CROP_INT_MASK;

	ctx->init_phase.w = SC_CROP_GET_FR_VAL(rect->width);
	if (ctx->init_phase.w) {
		rect->width &= SC_CROP_INT_MASK;
		rect->width += 1;
	}

	ctx->init_phase.h = SC_CROP_GET_FR_VAL(rect->height);
	if (ctx->init_phase.h) {
		rect->height &= SC_CROP_INT_MASK;
		rect->height += 1;
	}

	if (sc_fmt_is_yuv420(ctx->s_frame.sc_fmt->pixelformat)) {
		ctx->init_phase.ch = ctx->init_phase.yh / 2;
		ctx->init_phase.cv = ctx->init_phase.yv / 2;
	} else {
		ctx->init_phase.ch = ctx->init_phase.yh;
		ctx->init_phase.cv = ctx->init_phase.yv;
	}

	if ((ctx->init_phase.yh || ctx->init_phase.yv || ctx->init_phase.w ||
	     ctx->init_phase.h) &&
	     (!(sc_fmt_is_yuv420(ctx->s_frame.sc_fmt->pixelformat) ||
	     sc_fmt_is_rgb888(ctx->s_frame.sc_fmt->pixelformat)))) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "%s format on real number is not supported",
			 ctx->s_frame.sc_fmt->name);
		return -EINVAL;
	}

	v4l2_dbg(1, SC_LOG_LEVEL, &ctx->sc_dev->m2m.v4l2_dev,
		 "src crop position (x,y,w,h) = (%d.%d, %d.%d, %d.%d, %d.%d) %d, %d\n",
		 rect->left, ctx->init_phase.yh,
		 rect->top, ctx->init_phase.yv,
		 rect->width, ctx->init_phase.w,
		 rect->height, ctx->init_phase.h,
		 ctx->init_phase.ch, ctx->init_phase.cv);
	return 0;
}

static int sc_v4l2_s_selection(struct file *file, void *fh,
			       struct v4l2_selection *s)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(fh);
	struct sc_dev *sc = ctx->sc_dev;
	struct sc_frame *frame;
	struct v4l2_rect rect;
	const struct sc_size_limit *limit = NULL;
	int w_align = 0;
	int h_align = 0;
	int ret = 0;

	if (IS_ERR_OR_NULL(s)) {
		pr_err("[%s] s(%p) is wrong\n", __func__, s);
		return -EINVAL;
	}

	rect = s->r;
	frame = ctx_get_frame(ctx, s->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	if (!test_bit(CTX_PARAMS, &ctx->flags)) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "color format is not set\n");
		return -EINVAL;
	}

	if (s->r.left < 0 || s->r.top < 0) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "crop position is negative\n");
		return -EINVAL;
	}

	if (V4L2_TYPE_IS_OUTPUT(s->type)) {
		ret = sc_get_fract_val(&rect, ctx);
		if (ret < 0)
			return ret;
		limit = &sc->variant->limit_input;
		set_bit(CTX_SRC_FMT, &ctx->flags);
	} else {
		limit = &sc->variant->limit_output;
		set_bit(CTX_DST_FMT, &ctx->flags);
	}

	w_align = frame->sc_fmt->h_shift;
	h_align = frame->sc_fmt->v_shift;

	/* Bound an image to have crop width and height in limit */
	v4l_bound_align_image(&rect.width, limit->min_w, limit->max_w,
			      w_align, &rect.height, limit->min_h,
			      limit->max_h, h_align, 0);

	/* Bound an image to have crop position in limit */
	v4l_bound_align_image(&rect.left, 0, frame->width - rect.width,
			      w_align, &rect.top, 0, frame->height - rect.height,
			      h_align, 0);

	if (!V4L2_TYPE_IS_OUTPUT(s->type))
		rect.width = ALIGN_DOWN(rect.width, 4);

	if (rect.height > frame->height || rect.top > frame->height ||
	    rect.width > frame->width || rect.left > frame->width) {
		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "Out of crop range: (%d,%d,%d,%d) from %dx%d\n",
			 rect.left, rect.top, rect.width, rect.height,
			 frame->width, frame->height);
		return -EINVAL;
	}

	frame->crop.top = rect.top;
	frame->crop.left = rect.left;
	frame->crop.height = rect.height;
	frame->crop.width = rect.width;

	return 0;
}

static const struct v4l2_ioctl_ops sc_v4l2_ioctl_ops = {
	.vidioc_querycap		= sc_v4l2_querycap,

	.vidioc_g_fmt_vid_cap_mplane	= sc_v4l2_g_fmt_mplane,
	.vidioc_g_fmt_vid_out_mplane	= sc_v4l2_g_fmt_mplane,

	.vidioc_try_fmt_vid_cap_mplane	= sc_v4l2_try_fmt_mplane,
	.vidioc_try_fmt_vid_out_mplane	= sc_v4l2_try_fmt_mplane,

	.vidioc_s_fmt_vid_cap_mplane	= sc_v4l2_s_fmt_mplane,
	.vidioc_s_fmt_vid_out_mplane	= sc_v4l2_s_fmt_mplane,

	.vidioc_reqbufs			= sc_v4l2_reqbufs,
	.vidioc_querybuf		= sc_v4l2_querybuf,

	.vidioc_qbuf			= sc_v4l2_qbuf,
	.vidioc_dqbuf			= sc_v4l2_dqbuf,

	.vidioc_streamon		= sc_v4l2_streamon,
	.vidioc_streamoff		= sc_v4l2_streamoff,

	.vidioc_g_selection		= sc_v4l2_g_selection,
	.vidioc_s_selection		= sc_v4l2_s_selection,
};

struct v4l2_m2m_dev *sc_get_m2m_dev(struct sc_ctx *ctx)
{
	struct v4l2_m2m_dev *m2m_dev;
	struct sc_dev *sc = ctx->sc_dev;

	m2m_dev = (ctx->priority == SC_CTX_HIGH_PRIO ?
		sc->m2m.m2m_dev_hp : sc->m2m.m2m_dev_lp);

	return m2m_dev;
}

static int sc_ctx_stop_req(struct sc_ctx *ctx)
{
	struct sc_ctx *curr_ctx;
	struct sc_dev *sc = NULL;
	struct v4l2_m2m_dev *m2m_dev;
	int ret = 0;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return -EBUSY;
	}

	sc = ctx->sc_dev;
	m2m_dev = sc_get_m2m_dev(ctx);

	curr_ctx = v4l2_m2m_get_curr_priv(m2m_dev);
	if (!test_bit(CTX_RUN, &ctx->flags) || curr_ctx != ctx)
		return 0;

	set_bit(CTX_ABORT, &ctx->flags);

	ret = wait_event_timeout(sc->wait,
				 !test_bit(CTX_RUN, &ctx->flags), SC_TIMEOUT);

	/* TODO: How to handle case of timeout event */
	if (ret == 0) {
		dev_err(sc->dev, "device failed to stop request\n");
		ret = -EBUSY;
	}

	return ret;
}

static void sc_calc_planesize(struct sc_frame *frame, unsigned int pixsize)
{
	int idx = 0;

	if (IS_ERR_OR_NULL(frame)) {
		pr_err("[%s] frame(%p) is wrong\n", __func__, frame);
		return;
	}

	idx = frame->sc_fmt->num_planes;

	while (idx-- > 0)
		frame->addr.size[idx] =
			(pixsize * frame->sc_fmt->bitperpixel[idx]) / 8;
}

static int sc_prepare_2nd_scaling(struct sc_ctx *ctx,
				  __s32 src_width, __s32 src_height,
				  unsigned int *h_ratio, unsigned int *v_ratio)
{
	struct sc_dev *sc = ctx->sc_dev;
	struct v4l2_rect crop = ctx->d_frame.crop;
	const struct sc_size_limit *limit;
	unsigned int halign = 0, walign = 0;
	const struct sc_fmt *target_fmt = ctx->d_frame.sc_fmt;

	limit = &sc->variant->limit_input;
	if (*v_ratio > SCALE_RATIO_CONST(4, 1))
		crop.height = ((src_height + 7) / 8) * 2;

	if (crop.height < limit->min_h)
		crop.height = limit->min_h;

	if (*h_ratio > SCALE_RATIO_CONST(4, 1))
		crop.width = ((src_width + 7) / 8) * 2;

	if (crop.width < limit->min_w)
		crop.width = limit->min_w;

	if (*v_ratio < SCALE_RATIO_CONST(1, 8)) {
		crop.height = src_height * 8;
		if (crop.height > limit->max_h)
			crop.height = limit->max_h;
	}

	if (*h_ratio < SCALE_RATIO_CONST(1, 8)) {
		crop.width = src_width * 8;
		if (crop.width > limit->max_w)
			crop.width = limit->max_w;
	}

	walign = target_fmt->h_shift;
	halign = target_fmt->v_shift;

	limit = &sc->variant->limit_output;
	v4l_bound_align_image(&crop.width, limit->min_w, limit->max_w,
			      walign, &crop.height, limit->min_h,
			      limit->max_h, halign, 0);

	/* align up for scale down, align down for scale up */
	*h_ratio = SCALE_RATIO(src_width, crop.width);
	*v_ratio = SCALE_RATIO(src_height, crop.height);

	if (ctx->i_frame->frame.sc_fmt != ctx->d_frame.sc_fmt ||
	    memcmp(&crop, &ctx->i_frame->frame.crop, sizeof(crop)) ||
	    ctx->cp_enabled != test_bit(CTX_INT_FRAME_CP, &sc->state)) {
		if (ctx->cp_enabled)
			set_bit(CTX_INT_FRAME_CP, &sc->state);
		else
			clear_bit(CTX_INT_FRAME_CP, &sc->state);

		memcpy(&ctx->i_frame->frame, &ctx->d_frame,
		       sizeof(ctx->d_frame));
		memcpy(&ctx->i_frame->frame.crop, &crop, sizeof(crop));
	}

	return 0;
}

static struct sc_dnoise_filter sc_filter_tab[5] = {
	{SC_FT_240,   426,  240},
	{SC_FT_480,   854,  480},
	{SC_FT_720,  1280,  720},
	{SC_FT_960,  1920,  960},
	{SC_FT_1080, 1920, 1080},
};

static int sc_find_filter_size(struct sc_ctx *ctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sc_filter_tab); i++) {
		if (sc_filter_tab[i].strength == ctx->dnoise_ft.strength) {
			if (ctx->s_frame.width >= ctx->s_frame.height) {
				ctx->dnoise_ft.w = sc_filter_tab[i].w;
				ctx->dnoise_ft.h = sc_filter_tab[i].h;
			} else {
				ctx->dnoise_ft.w = sc_filter_tab[i].h;
				ctx->dnoise_ft.h = sc_filter_tab[i].w;
			}
			break;
		}
	}

	if (i == ARRAY_SIZE(sc_filter_tab)) {
		dev_err(ctx->sc_dev->dev,
			"%s: can't find filter size\n", __func__);
		return -EINVAL;
	}

	if (ctx->s_frame.crop.width < ctx->dnoise_ft.w ||
	    ctx->s_frame.crop.height < ctx->dnoise_ft.h) {
		dev_err(ctx->sc_dev->dev,
			"%s: filter is over source size.(%dx%d -> %dx%d)\n",
			__func__, ctx->s_frame.crop.width,
			ctx->s_frame.crop.height, ctx->dnoise_ft.w,
			ctx->dnoise_ft.h);
		return -EINVAL;
	}
	return 0;
}

static int sc_prepare_denoise_filter(struct sc_ctx *ctx)
{
	unsigned int sc_down_min = 0;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return -ENOMEM;
	}

	sc_down_min = ctx->sc_dev->variant->sc_down_min;

	if (ctx->dnoise_ft.strength <= SC_FT_BLUR)
		return 0;

	if (sc_find_filter_size(ctx))
		return -EINVAL;

	memcpy(&ctx->i_frame->frame, &ctx->d_frame, sizeof(ctx->d_frame));
	ctx->i_frame->frame.crop.width = ctx->dnoise_ft.w;
	ctx->i_frame->frame.crop.height = ctx->dnoise_ft.h;

	ctx->h_ratio = SCALE_RATIO(ctx->s_frame.crop.width, ctx->dnoise_ft.w);
	ctx->v_ratio = SCALE_RATIO(ctx->s_frame.crop.height, ctx->dnoise_ft.h);

	if (ctx->h_ratio > sc_down_min ||
	    ctx->h_ratio < ctx->sc_dev->variant->sc_up_max) {
		dev_err(ctx->sc_dev->dev,
			"filter can't support width scaling(%d -> %d)\n",
			ctx->s_frame.crop.width, ctx->dnoise_ft.w);
	}

	if (ctx->v_ratio > sc_down_min ||
	    ctx->v_ratio < ctx->sc_dev->variant->sc_up_max) {
		dev_err(ctx->sc_dev->dev,
			"filter can't support height scaling(%d -> %d)\n",
			ctx->s_frame.crop.height, ctx->dnoise_ft.h);
	}

	if (ctx->sc_dev->variant->prescale) {
		if (ctx->h_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_h_ratio = 2;
		else if (ctx->h_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_h_ratio = 1;
		else
			ctx->pre_h_ratio = 0;

		if (ctx->v_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_v_ratio = 2;
		else if (ctx->v_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_v_ratio = 1;
		else
			ctx->pre_v_ratio = 0;

		if (ctx->pre_h_ratio || ctx->pre_v_ratio) {
			if (!IS_ALIGNED(ctx->s_frame.crop.width,
					1 << (ctx->pre_h_ratio +
					ctx->s_frame.sc_fmt->h_shift))) {
				dev_err(ctx->sc_dev->dev,
					"filter can't support not-aligned source(%d -> %d)\n",
			ctx->s_frame.crop.width, ctx->dnoise_ft.w);
			} else if (!IS_ALIGNED(ctx->s_frame.crop.height,
					1 << (ctx->pre_v_ratio +
					ctx->s_frame.sc_fmt->v_shift))) {
				dev_err(ctx->sc_dev->dev,
					"filter can't support not-aligned source(%d -> %d)\n",
			ctx->s_frame.crop.height, ctx->dnoise_ft.h);
			} else {
				ctx->h_ratio >>= ctx->pre_h_ratio;
				ctx->v_ratio >>= ctx->pre_v_ratio;
			}
		}
	}

	return 0;
}

static int sc_find_scaling_ratio(struct sc_ctx *ctx)
{
	__s32 src_width, src_height;
	unsigned int h_ratio, v_ratio;
	struct sc_dev *sc = NULL;
	unsigned int sc_down_min = 0;
	unsigned int sc_up_max = 0;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return -EINVAL;
	}

	sc = ctx->sc_dev;
	sc_down_min = sc->variant->sc_down_min;
	sc_up_max = sc->variant->sc_up_max;

	if (ctx->s_frame.crop.width == 0 ||
	    ctx->d_frame.crop.width == 0)
		return 0; /* s_fmt is not complete */

	src_width = ctx->s_frame.crop.width;
	src_height = ctx->s_frame.crop.height;
	if (!!(ctx->flip_rot_cfg & SCALER_ROT_90))
		swap(src_width, src_height);

	h_ratio = SCALE_RATIO(src_width, ctx->d_frame.crop.width);
	v_ratio = SCALE_RATIO(src_height, ctx->d_frame.crop.height);

	/*
	 * If the source crop width or height is fractional value
	 * calculate scaling ratio including it and calculate with original
	 * crop.width and crop.height value because they were rounded up.
	 */
	if (ctx->init_phase.w)
		h_ratio = SCALE_RATIO_FRACT((src_width - 1), ctx->init_phase.w,
					    ctx->d_frame.crop.width);
	if (ctx->init_phase.h)
		v_ratio = SCALE_RATIO_FRACT((src_height - 1), ctx->init_phase.h,
					    ctx->d_frame.crop.height);
	sc_dbg("Scaling ratio h_ratio %d, v_ratio %d\n", h_ratio, v_ratio);

	if (h_ratio > sc->variant->sc_down_swmin ||
	    h_ratio < sc->variant->sc_up_swmax) {
		dev_err(sc->dev, "Width scaling is out of range(%d -> %d)\n",
			src_width, ctx->d_frame.crop.width);
		return -EINVAL;
	}

	if (v_ratio > sc->variant->sc_down_swmin ||
	    v_ratio < sc->variant->sc_up_swmax) {
		dev_err(sc->dev, "Height scaling is out of range(%d -> %d)\n",
			src_height, ctx->d_frame.crop.height);
		return -EINVAL;
	}

	if (sc->variant->prescale) {
		if (h_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_h_ratio = 2;
		else if (h_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_h_ratio = 1;
		else
			ctx->pre_h_ratio = 0;

		if (v_ratio > SCALE_RATIO_CONST(8, 1))
			ctx->pre_v_ratio = 2;
		else if (v_ratio > SCALE_RATIO_CONST(4, 1))
			ctx->pre_v_ratio = 1;
		else
			ctx->pre_v_ratio = 0;

		/*
		 * If the source image resolution violates the constraints of
		 * pre-scaler, then performs poly-phase scaling twice
		 */
		if (ctx->pre_h_ratio || ctx->pre_v_ratio) {
			if (!IS_ALIGNED(src_width, 1 << (ctx->pre_h_ratio +
					ctx->s_frame.sc_fmt->h_shift)) ||
				!IS_ALIGNED(src_height, 1 << (ctx->pre_v_ratio +
					ctx->s_frame.sc_fmt->v_shift))) {
				sc_down_min = SCALE_RATIO_CONST(4, 1);
				ctx->pre_h_ratio = 0;
				ctx->pre_v_ratio = 0;
			} else {
				h_ratio >>= ctx->pre_h_ratio;
				v_ratio >>= ctx->pre_v_ratio;
			}
		}

		if (sc_down_min == SCALE_RATIO_CONST(4, 1)) {
			dev_info(sc->dev,
				 "%s: Prepared 2nd polyphase scaler (%dx%d->%dx%d)\n",
				 __func__,
				 ctx->s_frame.crop.width, ctx->s_frame.crop.height,
				 ctx->d_frame.crop.width, ctx->d_frame.crop.height);
		}
	}

	if (h_ratio > sc_down_min || v_ratio > sc_down_min ||
	    h_ratio < sc_up_max || v_ratio < sc_up_max) {
		int ret;

		ret = sc_prepare_2nd_scaling(ctx, src_width, src_height,
					     &h_ratio, &v_ratio);
		if (ret)
			return ret;
	}

	ctx->h_ratio = h_ratio;
	ctx->v_ratio = v_ratio;

	return 0;
}

static int sc_vb2_queue_setup(struct vb2_queue *vq,
			      unsigned int *num_buffers, unsigned int *num_planes,
			      unsigned int sizes[], struct device *alloc_devs[])
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	struct sc_frame *frame;
	int ret;
	int i;

	frame = ctx_get_frame(ctx, vq->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	/* Get number of planes from format_list in driver */
	*num_planes = frame->sc_fmt->num_planes;
	for (i = 0; i < frame->sc_fmt->num_planes; i++) {
		if (frame->bytesused[i] == 0) {
			v4l2_err(&ctx->sc_dev->m2m.v4l2_dev, "not supported VIDIOC_REQBUFS before VIDIOC_S_FMT!\n");
			return -EINVAL;
		}
		sizes[i] = frame->bytesused[i];
		alloc_devs[i] = ctx->sc_dev->dev;
	}

	ret = sc_find_scaling_ratio(ctx);
	if (ret)
		return ret;

	ret = sc_prepare_denoise_filter(ctx);
	if (ret)
		return ret;

	return 0;
}

static int sc_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct sc_frame *frame;
	enum dma_data_direction dir;
	struct dma_buf *dbuf;
	int i;

	frame = ctx_get_frame(ctx, vb->vb2_queue->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	if (!V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		for (i = 0; i < frame->sc_fmt->num_planes; i++)
			vb2_set_plane_payload(vb, i, frame->bytesused[i]);
	}

	if (vb->vb2_queue->memory == VB2_MEMORY_DMABUF) {
		dir = V4L2_TYPE_IS_OUTPUT(vb->type) ?
				DMA_TO_DEVICE : DMA_FROM_DEVICE;

		for (i = 0; i < vb->num_planes; i++)
			dbuf = vb->planes[i].dbuf;
	}
	return 0;
}

static void sc_vb2_buf_finish(struct vb2_buffer *vb)
{
	struct vb2_sc_buffer *sc_buf = sc_from_vb2_to_sc_buf(vb);
	struct dma_fence *fence;
	struct vb2_queue *vq = vb->vb2_queue;
	struct dma_buf *dbuf;
	int i;

	do {
		fence = sc_buf->in_fence;
	} while (cmpxchg(&sc_buf->in_fence, fence, NULL) != fence);

	if (fence) {
		dma_fence_remove_callback(fence, &sc_buf->fence_cb);
		dma_fence_put(fence);
	} else if (work_busy(&sc_buf->qbuf_work)) {
		cancel_work_sync(&sc_buf->qbuf_work);
	}

	if (sc_buf->out_sync_file)
		sc_remove_out_fence(sc_buf);

	if (vb->vb2_queue->memory == VB2_MEMORY_DMABUF) {
		if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			for (i = 0; i < vb->num_planes; i++)
				dbuf = vb->planes[i].dbuf;
		}
	}
}

static void __sc_vb2_buf_queue(struct v4l2_m2m_ctx *m2m_ctx,
			       struct vb2_v4l2_buffer *v4l2_buf)
{
	v4l2_m2m_buf_queue(m2m_ctx, v4l2_buf);
	v4l2_m2m_try_schedule(m2m_ctx);
}

static void sc_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct sc_ctx *ctx = NULL;
	struct vb2_v4l2_buffer *v4l2_buf = NULL;
	struct vb2_sc_buffer *sc_buf = NULL;

	if (IS_ERR_OR_NULL(vb)) {
		pr_err("[%s] vb(%p) is wrong\n", __func__, vb);
		return;
	}

	ctx = vb2_get_drv_priv(vb->vb2_queue);
	v4l2_buf = to_vb2_v4l2_buffer(vb);
	sc_buf = sc_from_vb2_to_sc_buf(vb);

	if (sc_buf->in_fence) {
		int ret;

		ret = dma_fence_add_callback(sc_buf->in_fence,
					     &sc_buf->fence_cb, sc_fence_cb);
		if (ret) {
			dma_fence_put(sc_buf->in_fence);
			sc_buf->in_fence = NULL;
			if (ret != -ENOENT) {
				dev_err(ctx->sc_dev->dev,
					"%s: failed to add fence_cb[err:%d]\n",
					__func__, ret);
				sc_buf->state = ret;
			}
		} else {
			timer_setup(&sc_buf->fence_timer,
				    sc_fence_timeout_handler, 0);
			mod_timer(&sc_buf->fence_timer,
				  jiffies + msecs_to_jiffies(SC_FENCE_TIMEOUT));

			return;
		}
	}

	__sc_vb2_buf_queue(ctx->m2m_ctx, v4l2_buf);
}

static void sc_vb2_buf_cleanup(struct vb2_buffer *vb)
{
}

static void sc_vb2_lock(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);

	mutex_lock(&ctx->sc_dev->lock);
}

static void sc_vb2_unlock(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);

	mutex_unlock(&ctx->sc_dev->lock);
}

static int sc_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);

	set_bit(CTX_STREAMING, &ctx->flags);
	return 0;
}

static void sc_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct sc_ctx *ctx = vb2_get_drv_priv(vq);
	int ret;

	ret = sc_ctx_stop_req(ctx);
	if (ret < 0)
		dev_err(ctx->sc_dev->dev, "wait timeout\n");

	clear_bit(CTX_STREAMING, &ctx->flags);
}

static int sc_vb2_buf_init(struct vb2_buffer *vb)
{
	struct vb2_sc_buffer *sc_buf = sc_from_vb2_to_sc_buf(vb);

	INIT_WORK(&sc_buf->qbuf_work, __sc_qbuf_work);
	return 0;
}

static const struct vb2_ops sc_vb2_ops = {
	.queue_setup		= sc_vb2_queue_setup,
	.buf_init		= sc_vb2_buf_init,
	.buf_prepare		= sc_vb2_buf_prepare,
	.buf_finish		= sc_vb2_buf_finish,
	.buf_queue		= sc_vb2_buf_queue,
	.buf_cleanup		= sc_vb2_buf_cleanup,
	.wait_finish		= sc_vb2_lock,
	.wait_prepare		= sc_vb2_unlock,
	.start_streaming	= sc_vb2_start_streaming,
	.stop_streaming		= sc_vb2_stop_streaming,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct sc_ctx *ctx = priv;
	int ret;

	if (IS_ERR_OR_NULL(src_vq) || IS_ERR_OR_NULL(dst_vq)) {
		pr_err("[%s] src_vq(%p) or dst_vq(%p) is wrong\n", __func__, src_vq, dst_vq);
		return -EINVAL;
	}

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->ops = &sc_vb2_ops;
	src_vq->mem_ops = &vb2_dma_sg_memops;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct vb2_sc_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->ops = &sc_vb2_ops;
	dst_vq->mem_ops = &vb2_dma_sg_memops;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct vb2_sc_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	return vb2_queue_init(dst_vq);
}

static bool sc_configure_rotation_degree(struct sc_ctx *ctx, int degree)
{
	ctx->flip_rot_cfg &= ~SCALER_ROT_MASK;

	/*
	 * we expect that the direction of rotation is clockwise
	 * but the Scaler does in counter clockwise.
	 * Since the GScaler doest that in clockwise,
	 * the following makes the direction of rotation by the Scaler
	 * clockwise.
	 */
	if (degree == 270) {
		ctx->flip_rot_cfg |= SCALER_ROT_90;
	} else if (degree == 180) {
		ctx->flip_rot_cfg |= SCALER_ROT_180;
	} else if (degree == 90) {
		ctx->flip_rot_cfg |= SCALER_ROT_270;
	} else if (degree != 0) {
		dev_err(ctx->sc_dev->dev,
			"%s: Rotation of %d is not supported\n",
			__func__, degree);
		return false;
	}

	return true;
}

static void sc_set_framerate(struct sc_ctx *ctx, int framerate)
{
	if (!ctx->sc_dev->qos_table)
		return;

	if (framerate == 0)
		ctx->framerate = 0;
	else
		ctx->framerate = framerate;
}

static void sc_set_src_cspan(struct sc_ctx *ctx, int align)
{
	if (align < ALIGN_RESERVED)
		ctx->s_frame.cspanalign = align;
	else
		ctx->s_frame.cspanalign = 0;
}

static void sc_set_dst_cspan(struct sc_ctx *ctx, int align)
{
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	if (align < ALIGN_RESERVED)
		ctx->d_frame.cspanalign = align;
	else
		ctx->d_frame.cspanalign = 0;
}

static void sc_set_src_yspan(struct sc_ctx *ctx, int align)
{
	if (align < ALIGN_RESERVED)
		ctx->s_frame.yspanalign = align;
	else
		ctx->s_frame.yspanalign = 0;
}

static void sc_set_dst_yspan(struct sc_ctx *ctx, int align)
{
	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	if (align < ALIGN_RESERVED)
		ctx->d_frame.yspanalign = align;
	else
		ctx->d_frame.yspanalign = 0;
}

static int sc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc_ctx *ctx;
	struct sc_dev *sc;
	struct v4l2_m2m_dev *m2m_dev;
	int ret = 0;

	sc_dbg("ctrl ID:%d, value:%d\n", ctrl->id, ctrl->val);
	ctx = container_of(ctrl->handler, struct sc_ctx, ctrl_handler);
	sc = ctx->sc_dev;

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		if (ctrl->val)
			ctx->flip_rot_cfg |= SCALER_FLIP_X_EN;
		else
			ctx->flip_rot_cfg &= ~SCALER_FLIP_X_EN;
		break;
	case V4L2_CID_HFLIP:
		if (ctrl->val)
			ctx->flip_rot_cfg |= SCALER_FLIP_Y_EN;
		else
			ctx->flip_rot_cfg &= ~SCALER_FLIP_Y_EN;
		break;
	case V4L2_CID_ROTATE:
		if (!sc_configure_rotation_degree(ctx, ctrl->val))
			return -EINVAL;
		break;
	case V4L2_CID_GLOBAL_ALPHA:
		ctx->g_alpha = ctrl->val;
		break;
	case V4L2_CID_2D_BLEND_OP:
		if (!ctx->sc_dev->variant->blending && ctrl->val > 0) {
			dev_err(ctx->sc_dev->dev,
				"%s: blending is not supported from v2.2.0\n",
				__func__);
			return -EINVAL;
		}
		ctx->bl_op = ctrl->val;
		break;
	case V4L2_CID_2D_FMT_PREMULTI:
		ctx->pre_multi = ctrl->val;
		break;
	case V4L2_CID_2D_DITH:
		ctx->dith = ctrl->val;
		break;
	case V4L2_CID_CSC_EQ:
		ctx->csc.csc_eq = ctrl->val;
		break;
	case V4L2_CID_CSC_RANGE:
		ctx->csc.csc_range = ctrl->val;
		break;
	case V4L2_CID_CONTENT_PROTECTION:
		ctx->cp_enabled = !!ctrl->val;
		break;
	case SC_CID_DNOISE_FT:
		ctx->dnoise_ft.strength = ctrl->val;
		break;
	case SC_CID_FRAMERATE:
		sc_set_framerate(ctx, ctrl->val);
		break;
	case V4L2_SC_CID_SRC_CSPAN:
		sc_set_src_cspan(ctx, ctrl->val);
		break;
	case V4L2_SC_CID_DST_CSPAN:
		sc_set_dst_cspan(ctx, ctrl->val);
		break;
	case V4L2_SC_CID_SRC_YSPAN:
		sc_set_src_yspan(ctx, ctrl->val);
		break;
	case V4L2_SC_CID_DST_YSPAN:
		sc_set_dst_yspan(ctx, ctrl->val);
		break;

	case SC_CID_CTX_PRIORITY_SETTING:
		if (ctrl->val == SC_CTX_HIGH_PRIO) {
			m2m_dev = sc->m2m.m2m_dev_hp;

			v4l2_m2m_ctx_release(ctx->m2m_ctx);
			ctx->m2m_ctx = NULL;

		} else {
			m2m_dev = sc->m2m.m2m_dev_lp;
		}
		if (!ctx->m2m_ctx) {
			ctx->m2m_ctx = v4l2_m2m_ctx_init(m2m_dev, ctx, queue_init);
			if (IS_ERR(ctx->m2m_ctx)) {
				ret = -EINVAL;
				break;
			}
		}
		ctx->priority = ctrl->val;
		break;
	case V4L2_CID_2D_COLOR_FILL:
		if (!ctx->sc_dev->variant->color_fill) {
			dev_err(ctx->sc_dev->dev,
				"%s: color fill is not supported\n",
				__func__);
			return -EINVAL;
		}

		if (ctrl->val >= 0) {
			sc_dbg("color filled s ctrl called 0x%08x\n", ctrl->val);
			ctx->color_fill |= ctrl->val;
			ctx->color_fill_enabled = true;
		}

		break;
	case V4L2_CID_2D_ALPHA_FILL:
		if (!ctx->sc_dev->variant->color_fill) {
			dev_err(ctx->sc_dev->dev,
				"%s: alpha color fill is not supported\n",
				__func__);
			return -EINVAL;
		}

		if (ctrl->val >= 0) {
			sc_dbg("alpha filled s ctrl called 0x%08x\n", ctrl->val);
			ctx->color_fill |= ctrl->val << SCALER_ALPHA_FILL_SHIFT;
			ctx->color_fill_enabled = true;
		}

		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops sc_ctrl_ops = {
	.s_ctrl = sc_s_ctrl,
};

static const struct v4l2_ctrl_config sc_custom_ctrl[] = {
	{
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_GLOBAL_ALPHA,
		.name = "Set constant src alpha",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = 255,
		.def = 255,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_BLEND_OP,
		.name = "set blend op",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = BL_OP_ADD,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_DITH,
		.name = "set dithering",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = false,
		.max = true,
		.def = false,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_FMT_PREMULTI,
		.name = "set pre-multiplied format",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = false,
		.max = true,
		.def = false,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CSC_EQ,
		.name = "Set CSC equation",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = V4L2_COLORSPACE_DEFAULT,
		.max = V4L2_COLORSPACE_BT2020,
		.def = V4L2_COLORSPACE_DEFAULT,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CSC_RANGE,
		.name = "Set CSC range",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = SC_CSC_NARROW,
		.max = SC_CSC_WIDE,
		.def = SC_CSC_NARROW,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_CONTENT_PROTECTION,
		.name = "Enable contents protection",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.flags = V4L2_CTRL_FLAG_SLIDER,
		.step = 1,
		.min = 0,
		.max = 1,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = SC_CID_DNOISE_FT,
		.name = "Enable denoising filter",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = 0,
		.max = SC_FT_MAX,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = SC_CID_FRAMERATE,
		.name = "Frame rate setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = 0,
		.max = SC_FRAMERATE_MAX,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_SC_CID_SRC_CSPAN,
		.name = "C span align setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = ALIGN_DEFAULT,
		.max = ALIGN_256BYTES,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_SC_CID_DST_CSPAN,
		.name = "C span align setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = ALIGN_DEFAULT,
		.max = ALIGN_256BYTES,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_SC_CID_SRC_YSPAN,
		.name = "y span align setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = ALIGN_DEFAULT,
		.max = ALIGN_256BYTES,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_SC_CID_DST_YSPAN,
		.name = "Y span align setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = ALIGN_DEFAULT,
		.max = ALIGN_256BYTES,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = SC_CID_CTX_PRIORITY_SETTING,
		.name = "context priority setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = SC_CTX_DEFAULT_PRIO,
		.max = SC_CTX_HIGH_PRIO,
		.def = 0,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_COLOR_FILL,
		.name = "color fill setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = -1,
		.max = INT_MAX,
		.def = -1,
	}, {
		.ops = &sc_ctrl_ops,
		.id = V4L2_CID_2D_ALPHA_FILL,
		.name = "alpha fill setting",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.step = 1,
		.min = -1,
		.max = INT_MAX,
		.def = -1,
	}
};

static int sc_add_ctrls(struct sc_ctx *ctx)
{
	unsigned long i;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, SC_MAX_CTRL_NUM);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sc_ctrl_ops,
			  V4L2_CID_ROTATE, 0, 270, 90, 0);

	for (i = 0; i < ARRAY_SIZE(sc_custom_ctrl); i++)
		v4l2_ctrl_new_custom(&ctx->ctrl_handler,
				     &sc_custom_ctrl[i], NULL);
	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_err(&ctx->sc_dev->m2m.v4l2_dev,
			 "v4l2_ctrl_handler_init failed %d\n", err);
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	return 0;
}

static int sc_power_clk_enable(struct sc_dev *sc)
{
	int ret;

	if (IS_ERR_OR_NULL(sc)) {
		pr_err("[%s] sc(%p) is wrong\n", __func__, sc);
		return -EINVAL;
	}

	if (in_interrupt())
		ret = pm_runtime_get(sc->dev);
	else
		ret = pm_runtime_get_sync(sc->dev);

	if (ret < 0) {
		dev_err(sc->dev,
			"%s=%d: Failed to enable local power\n", __func__, ret);
		return ret;
	}

	if (!IS_ERR(sc->pclk)) {
		ret = clk_enable(sc->pclk);
		if (ret) {
			dev_err(sc->dev, "%s: Failed to enable PCLK (err %d)\n",
				__func__, ret);
			goto err_pclk;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_enable(sc->aclk);
		if (ret) {
			dev_err(sc->dev, "%s: Failed to enable ACLK (err %d)\n",
				__func__, ret);
			goto err_aclk;
		}
	}

	return 0;
err_aclk:
	if (!IS_ERR(sc->pclk))
		clk_disable(sc->pclk);
err_pclk:
	pm_runtime_put(sc->dev);
	return ret;
}

static void sc_clk_power_disable(struct sc_dev *sc)
{
	if (IS_ERR_OR_NULL(sc)) {
		pr_err("[%s] sc(%p) is wrong\n", __func__, sc);
		return;
	}

	sc_clear_aux_power_cfg(sc);

	if (!IS_ERR(sc->aclk))
		clk_disable(sc->aclk);

	if (!IS_ERR(sc->pclk))
		clk_disable(sc->pclk);

	pm_runtime_put(sc->dev);
}

static int sc_open(struct file *file)
{
	struct sc_dev *sc = video_drvdata(file);
	struct sc_ctx *ctx;
	int ret;

	if (atomic_read(&sc->m2m.in_use) == SC_MAX_CTX_NUM) {
		dev_err(sc->dev, "scaler device is out of contexts\n");
		return -EBUSY;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);

	if (!ctx)
		return -ENOMEM;

	atomic_inc(&sc->m2m.in_use);
	ctx->context_type = SC_CTX_V4L2_TYPE;
	INIT_LIST_HEAD(&ctx->node);
	ctx->sc_dev = sc;

	/*
	 * The device context for mem2mem will setup in s_ctrl
	 */
	ctx->m2m_ctx = NULL;

	v4l2_fh_init(&ctx->fh, sc->m2m.vfd);
	ret = sc_add_ctrls(ctx);
	if (ret)
		goto err_fh;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	/* Default color format */
	ctx->s_frame.sc_fmt = &sc_formats[0];
	ctx->d_frame.sc_fmt = &sc_formats[0];

	if (!IS_ERR(sc->pclk)) {
		ret = clk_prepare(sc->pclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare PCLK(err %d)\n",
				__func__, ret);
			goto err_pclk_prepare;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_prepare(sc->aclk);
		if (ret) {
			dev_err(sc->dev, "%s: failed to prepare ACLK(err %d)\n",
				__func__, ret);
			goto err_aclk_prepare;
		}
	}

	ctx->pm_qos_lv = -1;

	return 0;

err_aclk_prepare:
	if (!IS_ERR(sc->pclk))
		clk_unprepare(sc->pclk);
err_pclk_prepare:
	v4l2_fh_del(&ctx->fh);
err_fh:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	atomic_dec(&sc->m2m.in_use);
	kfree(ctx);

	return ret;
}

static int sc_release(struct file *file)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);
	struct sc_dev *sc = ctx->sc_dev;

	sc_dbg("refcnt= %d", atomic_read(&sc->m2m.in_use));

	atomic_dec(&sc->m2m.in_use);

	v4l2_m2m_ctx_release(ctx->m2m_ctx);

	if (ctx->framerate)
		ctx->framerate = 0;

	if (!IS_ERR(sc->aclk))
		clk_unprepare(sc->aclk);
	if (!IS_ERR(sc->pclk))
		clk_unprepare(sc->pclk);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return 0;
}

static unsigned int sc_poll(struct file *file,
			    struct poll_table_struct *wait)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);

	return v4l2_m2m_poll(file, ctx->m2m_ctx, wait);
}

static int sc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sc_ctx *ctx = fh_to_sc_ctx(file->private_data);

	return v4l2_m2m_mmap(file, ctx->m2m_ctx, vma);
}

static const struct v4l2_file_operations sc_v4l2_fops = {
	.owner		= THIS_MODULE,
	.open		= sc_open,
	.release	= sc_release,
	.poll		= sc_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= sc_mmap,
};

static void sc_buffer_done(struct vb2_v4l2_buffer *vb,
			   enum vb2_buffer_state state)
{
	struct vb2_sc_buffer *sc_buf;

	v4l2_m2m_buf_done(vb, state);

	sc_buf = sc_from_vb2_to_sc_buf(&vb->vb2_buf);

	if (sc_buf->out_sync_file) {
		if (state == VB2_BUF_STATE_ERROR)
			dma_fence_set_error(sc_buf->out_sync_file->fence,
					    -EFAULT);
		dma_fence_signal(sc_buf->out_sync_file->fence);
	}
}

static void sc_job_finish(struct sc_dev *sc, struct sc_ctx *ctx)
{
	unsigned long flags;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	struct v4l2_m2m_dev *m2m_dev;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	m2m_dev = sc_get_m2m_dev(ctx);

	spin_lock_irqsave(&sc->slock, flags);

	if (ctx->context_type == SC_CTX_V4L2_TYPE) {
		ctx = v4l2_m2m_get_curr_priv(m2m_dev);
		if (!ctx || !ctx->m2m_ctx) {
			dev_err(sc->dev, "current ctx is NULL\n");
			spin_unlock_irqrestore(&sc->slock, flags);
			return;
		}
		clear_bit(CTX_RUN, &ctx->flags);

		src_vb = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
		dst_vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

		sc_buffer_done(src_vb, VB2_BUF_STATE_ERROR);
		sc_buffer_done(dst_vb, VB2_BUF_STATE_ERROR);

		v4l2_m2m_job_finish(m2m_dev, ctx->m2m_ctx);
	}

	spin_unlock_irqrestore(&sc->slock, flags);
}

static void sc_watchdog(struct timer_list *t)
{
	struct sc_dev *sc = NULL;
	struct sc_ctx *ctx;
	unsigned long flags;

	if (IS_ERR_OR_NULL(t)) {
		pr_err("[%s] t(%p) is wrong\n", __func__, t);
		return;
	}

	sc_dbg("timeout watchdog\n");
	if (atomic_read(&sc->wdt.cnt) >= SC_WDT_CNT) {
		sc_hwset_soft_reset(sc);

		atomic_set(&sc->wdt.cnt, 0);
		clear_bit(DEV_RUN, &sc->state);

		spin_lock_irqsave(&sc->ctxlist_lock, flags);
		ctx = sc->current_ctx;
		sc->current_ctx = NULL;
		spin_unlock_irqrestore(&sc->ctxlist_lock, flags);

		sc_job_finish(sc, ctx);
		sc_clk_power_disable(sc);
		return;
	}

	if (test_bit(DEV_RUN, &sc->state)) {
		atomic_inc(&sc->wdt.cnt);
		dev_err(sc->dev, "scaler is still running\n");
		mod_timer(&sc->wdt.timer, jiffies + SC_TIMEOUT);
	} else {
		sc_dbg("scaler finished job\n");
	}
}

static void sc_set_csc_coef(struct sc_ctx *ctx)
{
	struct sc_frame *s_frame, *d_frame;
	struct sc_dev *sc;
	enum sc_csc_idx idx;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	sc = ctx->sc_dev;
	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	if (s_frame->sc_fmt->is_rgb == d_frame->sc_fmt->is_rgb)
		idx = NO_CSC;
	else if (s_frame->sc_fmt->is_rgb)
		idx = CSC_R2Y;
	else
		idx = CSC_Y2R;

	sc_hwset_csc_coef(sc, idx, &ctx->csc);
}

static bool sc_process_2nd_stage(struct sc_dev *sc, struct sc_ctx *ctx)
{
	struct sc_frame *s_frame, *d_frame;
	const struct sc_size_limit *limit;
	unsigned int halign = 0, walign = 0;
	unsigned int pre_h_ratio = 0;
	unsigned int pre_v_ratio = 0;
	unsigned int h_ratio = SCALE_RATIO(1, 1);
	unsigned int v_ratio = SCALE_RATIO(1, 1);

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return false;
	}

	if (!test_bit(CTX_INT_FRAME, &ctx->flags))
		return false;

	s_frame = &ctx->i_frame->frame;
	d_frame = &ctx->d_frame;

	s_frame->addr.ioaddr[SC_PLANE_Y] = ctx->i_frame->src_addr.ioaddr[SC_PLANE_Y];
	s_frame->addr.ioaddr[SC_PLANE_CB] = ctx->i_frame->src_addr.ioaddr[SC_PLANE_CB];
	s_frame->addr.ioaddr[SC_PLANE_CR] = ctx->i_frame->src_addr.ioaddr[SC_PLANE_CR];

	walign = d_frame->sc_fmt->h_shift;
	halign = d_frame->sc_fmt->v_shift;

	limit = &sc->variant->limit_input;
	v4l_bound_align_image(&s_frame->crop.width, limit->min_w, limit->max_w,
			      walign, &s_frame->crop.height, limit->min_h,
			      limit->max_h, halign, 0);

	sc_hwset_src_image_format(sc, s_frame);
	sc_hwset_dst_image_format(sc, d_frame);
	sc_hwset_src_imgsize(sc, s_frame);
	sc_hwset_dst_imgsize(sc, d_frame);

	if (ctx->flip_rot_cfg & SCALER_ROT_90 &&
	    ctx->dnoise_ft.strength > SC_FT_BLUR) {
		h_ratio = SCALE_RATIO(s_frame->crop.height, d_frame->crop.width);
		v_ratio = SCALE_RATIO(s_frame->crop.width, d_frame->crop.height);
	} else {
		h_ratio = SCALE_RATIO(s_frame->crop.width, d_frame->crop.width);
		v_ratio = SCALE_RATIO(s_frame->crop.height, d_frame->crop.height);
	}

	pre_h_ratio = 0;
	pre_v_ratio = 0;

	if (!sc->variant->ratio_20bit) {
		/* No prescaler, 1/4 precision */
		h_ratio >>= 4;
		v_ratio >>= 4;
	}

	sc_hwset_hratio(sc, h_ratio, pre_h_ratio);
	sc_hwset_vratio(sc, v_ratio, pre_v_ratio);

	sc_hwset_polyphase_hcoef(sc, h_ratio, h_ratio, 0);
	sc_hwset_polyphase_vcoef(sc, v_ratio, v_ratio, 0);

	sc_hwset_src_pos(sc, s_frame->crop.left, s_frame->crop.top,
			 s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);
	sc_hwset_src_wh(sc, s_frame->crop.width, s_frame->crop.height,
			pre_h_ratio, pre_v_ratio,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);

	sc_hwset_dst_pos(sc, d_frame->crop.left, d_frame->crop.top);
	sc_hwset_dst_wh(sc, d_frame->crop.width, d_frame->crop.height);

	sc_hwset_src_addr(sc, s_frame);
	sc_hwset_dst_addr(sc, d_frame);

	if (ctx->flip_rot_cfg & SCALER_ROT_MASK &&
	    ctx->dnoise_ft.strength > SC_FT_BLUR)
		sc_hwset_flip_rotation(sc, ctx->flip_rot_cfg);
	else
		sc_hwset_flip_rotation(sc, 0);

	sc_hwset_start(sc);
	clear_bit(CTX_INT_FRAME, &ctx->flags);

	return true;
}

static void sc_set_dithering(struct sc_ctx *ctx)
{
	struct sc_dev *sc = NULL;
	unsigned int val = 0;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	sc = ctx->sc_dev;

	if (ctx->dith)
		val = sc_dith_val(1, 1, 1);

	sc_dbg("dither value is 0x%x\n", val);
	sc_hwset_dith(sc, val);
}

static void sc_set_initial_phase(struct sc_ctx *ctx)
{
	struct sc_dev *sc = NULL;

	if (IS_ERR_OR_NULL(ctx)) {
		pr_err("[%s] ctx(%p) is wrong\n", __func__, ctx);
		return;
	}

	sc = ctx->sc_dev;

	/* TODO: need to check scaling, csc, rot according to H/W Goude  */
	sc_hwset_src_init_phase(sc, &ctx->init_phase);
}

static int ctx_empty(struct sc_dev *sc)
{
	return (list_empty(&sc->ctx_list_high_prio) &&
		list_empty(&sc->ctx_list_low_prio));
}

static void sc_hwset_ratio_polyphase_pos(struct sc_ctx *ctx, struct sc_dev *sc)
{
	struct sc_frame *d_frame, *s_frame;
	unsigned int pre_h_ratio = 0;
	unsigned int pre_v_ratio = 0;
	unsigned int h_ratio = SCALE_RATIO(1, 1);
	unsigned int v_ratio = SCALE_RATIO(1, 1);
	unsigned int ch_ratio = SCALE_RATIO(1, 1);
	unsigned int cv_ratio = SCALE_RATIO(1, 1);
	unsigned int h_shift, v_shift;

	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	if (ctx->i_frame)
		d_frame = &ctx->i_frame->frame;

	h_ratio = ctx->h_ratio;
	v_ratio = ctx->v_ratio;
	pre_h_ratio = ctx->pre_h_ratio;
	pre_v_ratio = ctx->pre_v_ratio;

	if (!sc->variant->ratio_20bit) {
		/* No prescaler, 1/4 precision */
		h_ratio >>= 4;
		v_ratio >>= 4;
	}

	h_shift = s_frame->sc_fmt->h_shift;
	v_shift = s_frame->sc_fmt->v_shift;

	if (!!(ctx->flip_rot_cfg & SCALER_ROT_90)) {
		swap(pre_h_ratio, pre_v_ratio);
		swap(h_shift, v_shift);
	}

	if (h_shift < d_frame->sc_fmt->h_shift)
		ch_ratio = h_ratio * 2; /* chroma scaling down */
	else if (h_shift > d_frame->sc_fmt->h_shift)
		ch_ratio = h_ratio / 2; /* chroma scaling up */
	else
		ch_ratio = h_ratio;

	if (v_shift < d_frame->sc_fmt->v_shift)
		cv_ratio = v_ratio * 2; /* chroma scaling down */
	else if (v_shift > d_frame->sc_fmt->v_shift)
		cv_ratio = v_ratio / 2; /* chroma scaling up */
	else
		cv_ratio = v_ratio;

	sc_hwset_hratio(sc, h_ratio, pre_h_ratio);
	sc_hwset_vratio(sc, v_ratio, pre_v_ratio);

	sc_hwset_polyphase_hcoef(sc, h_ratio, ch_ratio,
				 ctx->dnoise_ft.strength);
	sc_hwset_polyphase_vcoef(sc, v_ratio, cv_ratio,
				 ctx->dnoise_ft.strength);

	sc_hwset_src_pos(sc, s_frame->crop.left, s_frame->crop.top,
			 s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);
	sc_hwset_src_wh(sc, s_frame->crop.width, s_frame->crop.height,
			pre_h_ratio, pre_v_ratio,
			s_frame->sc_fmt->h_shift, s_frame->sc_fmt->v_shift);

	sc_hwset_dst_pos(sc, d_frame->crop.left, d_frame->crop.top);
	sc_hwset_dst_wh(sc, d_frame->crop.width, d_frame->crop.height);
}

static void sc_hwset_operation(struct sc_ctx *ctx, struct sc_dev *sc)
{
	unsigned int cfg = 0;

	sc_set_dithering(ctx);

	if (ctx->bl_op)
		sc_hwset_blend(sc, ctx->bl_op, ctx->pre_multi, ctx->g_alpha);

	if (ctx->dnoise_ft.strength > SC_FT_BLUR)
		sc_hwset_flip_rotation(sc, 0);
	else
		sc_hwset_flip_rotation(sc, ctx->flip_rot_cfg);

	if (ctx->color_fill_enabled) {
		sc_hwset_color_fill(sc, ctx->color_fill);
	} else {
		cfg = readl(sc->regs + SCALER_CFG);
		cfg &= ~(SCALER_CFG_FILL_EN);
		writel(cfg, sc->regs + SCALER_CFG);
	}
}

static void sc_hwset_image(struct sc_ctx *ctx, struct sc_dev *sc)
{
	struct sc_frame *d_frame, *s_frame;

	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	if (ctx->i_frame) {
		set_bit(CTX_INT_FRAME, &ctx->flags);
		d_frame = &ctx->i_frame->frame;
	}

	sc_hwset_src_image_format(sc, s_frame);
	sc_hwset_dst_image_format(sc, d_frame);

	sc_hwset_pre_multi_format(sc, s_frame->pre_multi, d_frame->pre_multi);

	sc_hwset_src_imgsize(sc, s_frame);
	sc_hwset_dst_imgsize(sc, d_frame);

	sc_set_csc_coef(ctx);

	sc_hwset_ratio_polyphase_pos(ctx, sc);

	if (sc->variant->initphase)
		sc_set_initial_phase(ctx);

	sc_hwset_src_addr(sc, s_frame);
	sc_hwset_dst_addr(sc, d_frame);
}

static void sc_hwset(struct sc_ctx *ctx, struct sc_dev *sc)
{
	sc_hwset_clk_request(sc, true);
	sc_hwset_image(ctx, sc);
	sc_hwset_operation(ctx, sc);
	sc_hwset_int_en(sc);
}

static int sc_run_next_job(struct sc_dev *sc)
{
	struct sc_ctx *ctx;
	int ret;

	if (IS_ERR_OR_NULL(sc)) {
		pr_err("[%s] sc(%p) is wrong\n", __func__, sc);
		return -EINVAL;
	}

	ret = sc_power_clk_enable(sc);
	if (ret) {
		/*
		 * Failed to enable the power and the clock. Let's push the task
		 * again for the later retry.
		 */
		clear_bit(DEV_RUN, &sc->state);

		/*
		 * V4L2 mem2mem assumes that the tasks in device_run() are
		 * always succeed in processing in H/W while m2m1shot accepts
		 * failure in device_run(). m2m1shot2 returns failure to the
		 * users if devce_run() fails. To prevent returning failure to
		 * users and losing a task to run, we should assume that
		 * processing a task always succeeds.
		 */
		return 0;
	}

	sc_hwset(ctx, sc);
	sc_hwset_start(sc);

	return 0;
}

static irqreturn_t sc_irq_handler(int irq, void *priv)
{
	struct sc_dev *sc = priv;
	struct sc_ctx *ctx;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;
	struct v4l2_m2m_dev *m2m_dev;
	u32 irq_status;

	spin_lock(&sc->slock);

	ctx = sc->current_ctx;
	if (!ctx) {
		if (ctx_empty(sc)) {
			irq_status = sc_hwget_and_clear_irq_status(sc);
			spin_unlock(&sc->slock);
			return IRQ_HANDLED;
		}
	}

	m2m_dev = sc_get_m2m_dev(ctx);

	irq_status = sc_hwget_and_clear_irq_status(sc);

	if (SCALER_INT_OK(irq_status) && sc_process_2nd_stage(sc, ctx))
		goto isr_unlock;

	if (!SCALER_INT_OK(irq_status))
		sc_hwset_soft_reset(sc);

	sc_hwset_clk_request(sc, false);

	clear_bit(DEV_RUN, &sc->state);
	clear_bit(CTX_RUN, &ctx->flags);

	if (ctx->context_type == SC_CTX_V4L2_TYPE) {
		src_vb = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
		dst_vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

		sc_buffer_done(src_vb,
			       SCALER_INT_OK(irq_status) ? VB2_BUF_STATE_DONE
							 : VB2_BUF_STATE_ERROR);
		sc_buffer_done(dst_vb,
			       SCALER_INT_OK(irq_status) ? VB2_BUF_STATE_DONE
							 : VB2_BUF_STATE_ERROR);

		v4l2_m2m_job_finish(m2m_dev, ctx->m2m_ctx);

		/* Wake up from CTX_ABORT state */
		clear_bit(CTX_ABORT, &ctx->flags);
	}

	spin_lock(&sc->ctxlist_lock);
	sc->current_ctx = NULL;
	spin_unlock(&sc->ctxlist_lock);

	wake_up(&sc->wait);

	sc_run_next_job(sc);

	sc_clk_power_disable(sc);
isr_unlock:
	spin_unlock(&sc->slock);

	return IRQ_HANDLED;
}

static dma_addr_t sc_get_vb2_dma_addr(struct vb2_buffer *vb2buf, int plane_no)
{
	struct sg_table *sgt;

	sgt = vb2_dma_sg_plane_desc(vb2buf, plane_no);
	if (!sgt)
		return -ENOMEM;

	return sg_dma_address(sgt->sgl);
}

static int sc_get_bufaddr(struct sc_dev *sc, struct vb2_buffer *vb2buf,
			  struct sc_frame *frame)
{
	unsigned int pixsize, bytesize;

	pixsize = frame->width * frame->height;
	bytesize = (pixsize * frame->sc_fmt->bitperpixel[0]) >> 3;

	frame->addr.ioaddr[SC_PLANE_Y] = sc_get_vb2_dma_addr(vb2buf, 0);
	frame->addr.ioaddr[SC_PLANE_CB] = 0;
	frame->addr.ioaddr[SC_PLANE_CR] = 0;

	switch (frame->sc_fmt->num_comp) {
	case 1: /* rgb, yuyv */
		frame->addr.size[SC_PLANE_Y] = bytesize;
		frame->addr.size[SC_PLANE_CB] = 0;
		frame->addr.size[SC_PLANE_CR] = 0;
		break;
	case 2:
		frame->addr.size[SC_PLANE_CB] = 0;
		frame->addr.size[SC_PLANE_CR] = 0;
		if (frame->sc_fmt->num_planes == 1) {
			if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;

				frame->addr.ioaddr[SC_PLANE_CB] =
					NV12N_CBCR_BASE(frame->addr.ioaddr[SC_PLANE_Y], w, h);
				frame->addr.size[SC_PLANE_Y] = NV12N_Y_SIZE(w, h);
				frame->addr.size[SC_PLANE_CB] = NV12N_CBCR_SIZE(w, h);
			} else if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12N_10B) {
				unsigned int w = frame->width;
				unsigned int h = frame->height;

				frame->addr.ioaddr[SC_PLANE_CB] =
					NV12N_10B_CBCR_BASE(frame->addr.ioaddr[SC_PLANE_Y], w, h);
				frame->addr.size[SC_PLANE_Y] = NV12N_Y_SIZE(w, h);
				frame->addr.size[SC_PLANE_CB] = NV12N_CBCR_SIZE(w, h);
			} else {
				if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_NV12_P010)
					pixsize *= 2;
				frame->addr.ioaddr[SC_PLANE_CB] =
					frame->addr.ioaddr[SC_PLANE_Y] + pixsize;
				frame->addr.size[SC_PLANE_Y] = pixsize;
				frame->addr.size[SC_PLANE_CB] = bytesize - pixsize;
			}
		} else if (frame->sc_fmt->num_planes == 2) {
			frame->addr.ioaddr[SC_PLANE_CB] = sc_get_vb2_dma_addr(vb2buf, 1);
			sc_calc_planesize(frame, pixsize);
		}
		break;
	default:
		break;
	}

	if (frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420 ||
	    frame->sc_fmt->pixelformat == V4L2_PIX_FMT_YVU420M) {
		u32 t_cb = frame->addr.ioaddr[SC_PLANE_CB];

		frame->addr.ioaddr[SC_PLANE_CB] = frame->addr.ioaddr[SC_PLANE_CR];
		frame->addr.ioaddr[SC_PLANE_CR] = t_cb;
	}

	sc_dbg("y addr %pa y size %#x\n", &frame->addr.ioaddr[SC_PLANE_Y],
	       frame->addr.size[SC_PLANE_Y]);
	sc_dbg("cb addr %pa cb size %#x\n", &frame->addr.ioaddr[SC_PLANE_CB],
	       frame->addr.size[SC_PLANE_CB]);
	sc_dbg("cr addr %pa cr size %#x\n", &frame->addr.ioaddr[SC_PLANE_CR],
	       frame->addr.size[SC_PLANE_CR]);

	return 0;
}

static void sc_m2m_device_run(void *priv)
{
	struct sc_ctx *ctx = priv;
	struct sc_dev *sc = ctx->sc_dev;
	struct sc_frame *s_frame, *d_frame;
	struct vb2_buffer *src_vb, *dst_vb;
	struct vb2_v4l2_buffer *src_vb_v4l2, *dst_vb_v4l2;
	struct vb2_sc_buffer *src_sc_buf, *dst_sc_buf;
	struct v4l2_m2m_dev *m2m_dev;

	s_frame = &ctx->s_frame;
	d_frame = &ctx->d_frame;

	m2m_dev = sc_get_m2m_dev(ctx);

	src_vb = (struct vb2_buffer *)v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	dst_vb = (struct vb2_buffer *)v4l2_m2m_next_dst_buf(ctx->m2m_ctx);

	src_sc_buf = sc_from_vb2_to_sc_buf(src_vb);
	dst_sc_buf = sc_from_vb2_to_sc_buf(dst_vb);

	if (src_sc_buf->state || dst_sc_buf->state) {
		src_vb_v4l2 = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
		dst_vb_v4l2 = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

		sc_buffer_done(src_vb_v4l2, VB2_BUF_STATE_ERROR);
		sc_buffer_done(dst_vb_v4l2, VB2_BUF_STATE_ERROR);

		v4l2_m2m_job_finish(m2m_dev, ctx->m2m_ctx);
		return;
	}

	sc_get_bufaddr(sc, src_vb, s_frame);
	sc_get_bufaddr(sc, dst_vb, d_frame);
}

static void sc_m2m_job_abort(void *priv)
{
	struct sc_ctx *ctx = priv;
	int ret;

	ret = sc_ctx_stop_req(ctx);
	if (ret < 0)
		dev_err(ctx->sc_dev->dev, "wait timeout\n");
}

static struct v4l2_m2m_ops sc_m2m_ops = {
	.device_run	= sc_m2m_device_run,
	.job_abort	= sc_m2m_job_abort,
};

static void sc_unregister_m2m_device(struct sc_dev *sc)
{
	if (IS_ERR_OR_NULL(sc)) {
		pr_err("[%s] sc(%p) is wrong\n", __func__, sc);
		return;
	}

	v4l2_m2m_release(sc->m2m.m2m_dev_hp);
	v4l2_m2m_release(sc->m2m.m2m_dev_lp);
	video_unregister_device(sc->m2m.vfd);
	v4l2_device_unregister(&sc->m2m.v4l2_dev);
}

static int sc_register_m2m_device(struct sc_dev *sc, int dev_id)
{
	struct v4l2_device *v4l2_dev;
	struct device *dev;
	struct video_device *vfd;
	int ret = 0;

	dev = sc->dev;
	v4l2_dev = &sc->m2m.v4l2_dev;

	scnprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s.m2m",
		  MODULE_NAME);

	ret = v4l2_device_register(dev, v4l2_dev);
	if (ret) {
		dev_err(sc->dev, "failed to register v4l2 device\n");
		return ret;
	}

	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(sc->dev, "failed to allocate video device\n");
		goto err_v4l2_dev;
	}

	vfd->fops	= &sc_v4l2_fops;
	vfd->ioctl_ops	= &sc_v4l2_ioctl_ops;
	vfd->release	= video_device_release;
	vfd->lock	= &sc->lock;
	vfd->vfl_dir	= VFL_DIR_M2M;
	vfd->v4l2_dev	= v4l2_dev;
	vfd->device_caps =  V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING |
				V4L2_CAP_VIDEO_OUTPUT_MPLANE |
				V4L2_CAP_VIDEO_CAPTURE_MPLANE;
	scnprintf(vfd->name, sizeof(vfd->name), "%s:m2m", MODULE_NAME);

	video_set_drvdata(vfd, sc);

	sc->m2m.vfd = vfd;
	sc->m2m.m2m_dev_lp = v4l2_m2m_init(&sc_m2m_ops);
	if (IS_ERR(sc->m2m.m2m_dev_lp)) {
		dev_err(sc->dev, "failed to initialize v4l2-m2m low priority device\n");
		ret = PTR_ERR(sc->m2m.m2m_dev_lp);
		goto err_lp_dev_alloc;
	}

	sc->m2m.m2m_dev_hp = v4l2_m2m_init(&sc_m2m_ops);
	if (IS_ERR(sc->m2m.m2m_dev_hp)) {
		dev_err(sc->dev, "failed to initialize v4l2-m2m low priority device\n");
		ret = PTR_ERR(sc->m2m.m2m_dev_hp);
		goto err_hp_dev_alloc;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 50 + dev_id);
	if (ret) {
		dev_err(sc->dev, "failed to register video device (video%d)\n",
			50 + dev_id);
		goto err_m2m_dev;
	}

	return 0;

err_m2m_dev:
	v4l2_m2m_release(sc->m2m.m2m_dev_hp);
err_hp_dev_alloc:
	v4l2_m2m_release(sc->m2m.m2m_dev_lp);
err_lp_dev_alloc:
	video_device_release(sc->m2m.vfd);
err_v4l2_dev:
	v4l2_device_unregister(v4l2_dev);

	return ret;
}

#ifdef CONFIG_PM_SLEEP

static int sc_suspend(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);
	int ret;

	set_bit(DEV_SUSPEND, &sc->state);

	ret = wait_event_timeout(sc->wait,
				 !test_bit(DEV_RUN, &sc->state), SC_TIMEOUT);
	if (ret == 0)
		dev_err(sc->dev, "wait timeout\n");

	return 0;
}

static int sc_resume(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	clear_bit(DEV_SUSPEND, &sc->state);

	if (sc->version >= SCALER_VERSION(7, 0, 1) && !IS_ERR(sc->sysreg))
		writel(SCALER_LLC_NO_HINT, sc->sysreg + sc->sysreg_offset);

	return 0;
}
#endif

#ifdef CONFIG_PM

static int sc_runtime_resume(struct device *dev)
{
	struct sc_dev *sc = dev_get_drvdata(dev);

	if (!IS_ERR(sc->clk_chld) && !IS_ERR(sc->clk_parn)) {
		int ret = clk_set_parent(sc->clk_chld, sc->clk_parn);

		if (ret) {
			dev_err(sc->dev, "%s: Failed to setup MUX: %d\n",
				__func__, ret);
			return ret;
		}
	}

	return 0;
}

static int sc_runtime_suspend(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops sc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sc_suspend, sc_resume)
	SET_RUNTIME_PM_OPS(NULL, sc_runtime_resume, sc_runtime_suspend)
};

static int sc_compare_qos_table_entries(const void *p1, const void *p2)
{
	const struct sc_qos_table *t1 = NULL;
	const struct sc_qos_table *t2 = NULL;

	if (IS_ERR_OR_NULL(p1) || IS_ERR_OR_NULL(p2)) {
		pr_err("[%s] p1(%p) or p2(%p) is wrong\n", __func__, p1, p2);
		return -1;
	}

	t1 = p1;
	t2 = p2;

	if (t1->freq_int < t2->freq_int)
		return 1;
	else
		return -1;
}

static int sc_populate_dt(struct sc_dev *sc)
{
	struct device *dev = NULL;
	struct sc_qos_table *qos_table;
	struct sc_ppc_table *ppc_table;
	int i, len;

	if (IS_ERR_OR_NULL(sc)) {
		pr_err("[%s] sc(%p) is wrong\n", __func__, sc);
		return -ENOMEM;
	}

	dev = sc->dev;

	len = of_property_count_u32_elems(dev->of_node, "mscl_qos_table");
	if (len <= 0) {
		dev_info(dev, "No qos table for scaler\n");
		return 0;
	}

	sc->qos_table_cnt = len / 3;

	qos_table = devm_kzalloc(dev, sizeof(struct sc_qos_table) * sc->qos_table_cnt, GFP_KERNEL);
	if (!qos_table)
		return -ENOMEM;

	of_property_read_u32_array(dev->of_node, "mscl_qos_table",
				   (unsigned int *)qos_table, len);

	sort(qos_table, sc->qos_table_cnt, sizeof(*qos_table),
	     sc_compare_qos_table_entries, NULL);

	for (i = 0; i < sc->qos_table_cnt; i++) {
		dev_info(dev, "MSCL QoS Table[%d] mif : %u int : %u [%u]\n", i,
			 qos_table[i].freq_mif,
			 qos_table[i].freq_int,
			 qos_table[i].data_size);
	}

	sc->qos_table = qos_table;

	len = of_property_count_u32_elems(dev->of_node, "mscl_ppc_table");
	if (len <= 0) {
		dev_info(dev, "No ppc table for scaler\n");
		return -ENOENT;
	}

	sc->ppc_table_cnt = len / 3;

	ppc_table = devm_kzalloc(dev,
				 sizeof(*ppc_table) * sc->ppc_table_cnt, GFP_KERNEL);
	if (!ppc_table)
		return -ENOMEM;

	of_property_read_u32_array(dev->of_node, "mscl_ppc_table",
				   (unsigned int *)ppc_table, len);

	for (i = 0; i < sc->ppc_table_cnt; i++) {
		dev_info(dev, "MSCL PPC Table[%d] bpp : %u ppc : %u/%u\n", i,
			 ppc_table[i].bpp,
			 ppc_table[i].ppc[0],
			 ppc_table[i].ppc[1]);
	}

	sc->ppc_table = ppc_table;

	return 0;
}

static int sc_get_hwversion(struct sc_dev *sc)
{
	int ret = 0;
	size_t ivar;
	bool get_hwversion = false;
	u32 hwver = 0;

	ret = pm_runtime_get_sync(sc->dev);
	if (ret < 0) {
		dev_err(sc->dev, "%s: failed to local power on (err %d)\n",
			__func__, ret);
		goto err_ver_rpm_get;
	}

	if (!IS_ERR(sc->pclk)) {
		ret = clk_prepare_enable(sc->pclk);
		if (ret) {
			dev_err(sc->dev,
				"%s: failed to enable PCLK (err %d)\n",
				__func__, ret);
			goto err_ver_pclk_get;
		}
	}

	if (!IS_ERR(sc->aclk)) {
		ret = clk_prepare_enable(sc->aclk);
		if (ret) {
			dev_err(sc->dev,
				"%s: failed to enable ACLK (err %d)\n",
				__func__, ret);
			goto err_ver_aclk_get;
		}
	}

	sc->version = SCALER_VERSION(2, 0, 0);

	hwver = __raw_readl(sc->regs + SCALER_VER);

	/* selects the lowest version number if no version is matched */
	for (ivar = 0; ivar < ARRAY_SIZE(sc_version_table); ivar++) {
		sc->version = sc_version_table[ivar][1];
		if (hwver == sc_version_table[ivar][0]) {
			get_hwversion = true;
			ret = hwver;
			break;
		}
	}

	if (!get_hwversion) {
		dev_err(sc->dev,
			"Could not support this hw in driver(version: %08x)\n", hwver);
		ret = -EINVAL;
		goto err_ver_aclk_get;
	}

	return ret;
err_ver_aclk_get:
	if (!IS_ERR(sc->pclk))
		clk_disable_unprepare(sc->pclk);
err_ver_pclk_get:
	pm_runtime_put(sc->dev);
err_ver_rpm_get:
	sc_unregister_m2m_device(sc);
	return ret;
}

static int sc_probe(struct platform_device *pdev)
{
	struct sc_dev *sc;
	struct resource *res;
	int ret = 0;
	size_t ivar;
	u32 hwver = 0;
	int irq_num;

	sc = devm_kzalloc(&pdev->dev, sizeof(struct sc_dev), GFP_KERNEL);
	if (!sc)
		goto err_dev;

	sc->dev = &pdev->dev;
	spin_lock_init(&sc->ctxlist_lock);
	INIT_LIST_HEAD(&sc->ctx_list_high_prio);
	INIT_LIST_HEAD(&sc->ctx_list_low_prio);
	spin_lock_init(&sc->slock);
	mutex_init(&sc->lock);
	init_waitqueue_head(&sc->wait);

	sc->fence_context = dma_fence_context_alloc(1);
	spin_lock_init(&sc->fence_lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pr_err("Resource start: 0x%pa, end: 0x%pa, size: 0x%lx, flags: 0x%lx\n",
	       &res->start, &res->end,
	       (unsigned long)resource_size(res),
	       (unsigned long)res->flags);
	sc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sc->regs)) {
		pr_err("devm_ioremap_resource failed: %pe\n", sc->regs);
		goto err_io_resource;
	}
	dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));

	atomic_set(&sc->wdt.cnt, 0);
	timer_setup(&sc->wdt.timer, sc_watchdog, 0);

	if (pdev->dev.of_node) {
		sc->dev_id = of_alias_get_id(pdev->dev.of_node, "scaler");
		if (sc->dev_id < 0) {
			dev_err(&pdev->dev,
				"Failed to read scaler node id(%d)!\n", sc->dev_id);
			ret = -EINVAL;
			goto err_node_id;
		}
	} else {
		sc->dev_id = pdev->id;
	}

	platform_set_drvdata(pdev, sc);

	pm_runtime_enable(&pdev->dev);

	ret = sc_populate_dt(sc);
	if (ret)
		goto err_dt;

	ret = sc_register_m2m_device(sc, sc->dev_id);
	if (ret) {
		dev_err(&pdev->dev, "failed to register m2m device\n");
		goto err_m2m;
	}

#if defined(CONFIG_PM_DEVFREQ) && defined(NEVER_DEFINED)
	if (!of_property_read_u32(pdev->dev.of_node, "mscl,int_qos_minlock",
				  (u32 *)&sc->qosreq_int_level)) {
		if (sc->qosreq_int_level > 0) {
			exynos_pm_qos_add_request(&sc->qosreq_int,
						  PM_QOS_DEVICE_THROUGHPUT, 0);
			dev_info(&pdev->dev, "INT Min.Lock Freq. = %u\n",
				 sc->qosreq_int_level);
		}
	}
#endif
	if (of_property_read_u32(pdev->dev.of_node, "mscl,cfw",
				 (u32 *)&sc->cfw))
		sc->cfw = 0;

	ret = sc_get_hwversion(sc);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: failed to get hw version (err %d)\n",
			__func__, ret);
		goto err_m2m;
	} else {
		hwver = ret;
	}

	for (ivar = 0; ivar < ARRAY_SIZE(sc_variant); ivar++) {
		if (sc->version >= sc_variant[ivar].version) {
			sc->variant = &sc_variant[ivar];
			break;
		}
	}

	if (sc->version >= SCALER_VERSION(7, 0, 1)) {
		sc->sysreg_offset = SCALER_SYSREG_OFFSET(res->start);
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (res) {
			sc->sysreg = devm_ioremap_resource(&pdev->dev, res);
			if (IS_ERR(sc->sysreg)) {
				dev_info(&pdev->dev, "SCALER LLC SYSREG is not setted.\n");
			} else {
				writel(SCALER_LLC_NO_HINT, sc->sysreg + sc->sysreg_offset);
				dev_info(&pdev->dev, "SCALER LLC SYSREG is setted with NO_HINT.\n");
			}
		}
	}

	sc_hwset_soft_reset(sc);

	if (!IS_ERR(sc->aclk))
		clk_disable_unprepare(sc->aclk);
	if (!IS_ERR(sc->pclk))
		clk_disable_unprepare(sc->pclk);
	pm_runtime_put(&pdev->dev);

	irq_num = platform_get_irq(pdev, 0);
	if (irq_num < 0) {
		dev_err(&pdev->dev, "failed to get IRQ resource\n");
		ret = -ENOENT;
		goto err_get_irq_res;
	}

	ret = devm_request_irq(&pdev->dev, irq_num, sc_irq_handler, 0,
			       pdev->name, sc);
	if (ret) {
		dev_err(&pdev->dev, "failed to install irq\n");
		goto err_request_irq;
	}

	dev_info(&pdev->dev,
		 "Driver probed successfully(version: %08x(%x))\n",
		 hwver, sc->version);

	return 0;

err_request_irq:
err_get_irq_res:
err_m2m:
err_dt:
err_node_id:
err_io_resource:
	if (sc)
		devm_kfree(&pdev->dev, sc);
err_dev:
	dev_err(&pdev->dev,
		"Driver probed failed!\n");

	return ret;
}

static void sc_remove(struct platform_device *pdev)
{
	struct sc_dev *sc = platform_get_drvdata(pdev);

	sc_unregister_m2m_device(sc);
}

static void sc_shutdown(struct platform_device *pdev)
{
	struct sc_dev *sc = platform_get_drvdata(pdev);

	set_bit(DEV_SUSPEND, &sc->state);

	wait_event(sc->wait,
		   !test_bit(DEV_RUN, &sc->state));
}

static const struct of_device_id exynos_sc_match[] = {
	{
		.compatible = "samsung,exynos5-scaler",
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_sc_match);

static struct platform_driver sc_driver = {
	.probe		= sc_probe,
	.remove		= sc_remove,
	.shutdown	= sc_shutdown,
	.driver = {
		.name	= MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &sc_pm_ops,
		.of_match_table = of_match_ptr(exynos_sc_match),
	}
};

module_platform_driver(sc_driver);

MODULE_DESCRIPTION("EXYNOS m2m scaler driver");
MODULE_LICENSE("GPL");
