/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Bo Kong <bo.kong@mediatek.com>
 */

#ifndef __MTK_AIE_H__
#define __MTK_AIE_H__

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/mtk_aie_v4l2_controls.h>

#define RPN_NUM				3
#define RPN_LOOP_NUM			29
#define FD_LOOP_NUM			(RPN_NUM * RPN_LOOP_NUM)

#define RPNX_LOOP_NUM(x)		((3 - (x)) * RPN_LOOP_NUM - 1)
#define PYMX_START_LOOP(x)		((2 - (x)) * RPN_LOOP_NUM)

#define ATTR_LOOP_NUM			26
#define AGE_OUT_RGS			17
#define GENDER_OUT_RGS			20
#define INDIAN_OUT_RGS			22
#define ETHNICITY_OUT_RGS			25

#define INPUT_WDMA_WRA_NUM		4
#define OUTPUT_WDMA_WRA_NUM		4
#define KERNEL_RDMA_RA_NUM		2

#define MAX_ENQUE_FRAME_NUM		10
#define PYM_NUM				3
#define COLOR_NUM			3

#define ATTR_MODE_PYRAMID_WIDTH		128
#define ATTR_OUT_SIZE			32

#define AIE_START_REG			0x000
#define AIE_ENABLE_REG			0x004
#define AIE_LOOP_REG			0x008
#define AIE_YUV2RGB_CON_BASE_ADR_REG	0x00c
#define AIE_RS_CON_BASE_ADR_REG		0x010
#define AIE_FD_CON_BASE_ADR_REG		0x014
#define AIE_INT_EN_REG			0x018
#define AIE_INT_REG			0x01c
#define AIE_RESULT_0_REG		0x08c
#define AIE_RESULT_1_REG		0x090
#define AIE_DMA_CTL_REG			0x094

/* AIE 3.0 register */
#define AIE_YUV2RGB_CON_BASE_ADR_MSB	0x14C
#define AIE_RS_CON_BASE_ADR_MSB		0x150
#define AIE_FD_CON_BASE_ADR_MSB		0x154

/* AIE 3.0 FLD register */
#define FLD_EN				0x400
#define FLD_BASE_ADDR_FACE_0		0x404
#define FLD_MODEL_PARA1			0x4FC
#define FLD_MODEL_PARA14		0x530

#define FLD_BUSY			0x544
#define FLD_DONE			0x548
#define FLD_SRC_WD_HT			0x54C

/* n: min 0, max 14 */
#define FLD_PL_IN_BASE_ADDR_0_(n)	(0x550 + 4 * (n))
#define FLD_PL_IN_BASE_ADDR_1_(n)	(0x5C8 + 4 * (n))
#define FLD_PL_IN_BASE_ADDR_2_(n)	(0x640 + 4 * (n))
#define FLD_PL_IN_BASE_ADDR_3_(n)	(0x6B8 + 4 * (n))
#define FLD_SH_IN_BASE_ADDR_(n)		(0x85C + 4 * (n))

#define FLD_PL_IN_SIZE_0		0x730
#define FLD_PL_IN_STRIDE_0		0x734
#define FLD_PL_IN_SIZE_1		0x738
#define FLD_PL_IN_STRIDE_1		0x73C
#define FLD_PL_IN_SIZE_2_0		0x740
#define FLD_PL_IN_STRIDE_2_0		0x744
#define FLD_PL_IN_SIZE_2_1		0x748
#define FLD_PL_IN_STRIDE_2_1		0x74C
#define FLD_PL_IN_SIZE_2_2		0x750
#define FLD_PL_IN_STRIDE_2_2		0x754
#define FLD_PL_IN_SIZE_3		0x758
#define FLD_PL_IN_STRIDE_3		0x75C

#define FLD_SH_IN_SIZE_0		0x7D8
#define FLD_SH_IN_STRIDE_0		0x7DC
#define FLD_TR_OUT_BASE_ADDR_0		0x7E0
#define FLD_TR_OUT_SIZE_0		0x7E4
#define FLD_TR_OUT_STRIDE_0		0x7E8
#define FLD_PP_OUT_BASE_ADDR_0		0x7EC
#define FLD_PP_OUT_SIZE_0		0x7F0
#define FLD_PP_OUT_STRIDE_0		0x7F4
#define FLD_SPARE			0x7F8

#define FLD_BASE_ADDR_FACE_0_7_MSB	0x7FC
#define FLD_BASE_ADDR_FACE_8_14_MSB	0x800

#define FLD_PL_IN_BASE_ADDR_0_0_7_MSB	0x804
#define FLD_PL_IN_BASE_ADDR_0_8_15_MSB	0x808
#define FLD_PL_IN_BASE_ADDR_0_16_23_MSB	0x80C
#define FLD_PL_IN_BASE_ADDR_0_24_29_MSB	0x810

#define FLD_PL_IN_BASE_ADDR_1_0_7_MSB	0x814
#define FLD_PL_IN_BASE_ADDR_1_8_15_MSB	0x818
#define FLD_PL_IN_BASE_ADDR_1_16_23_MSB	0x81C
#define FLD_PL_IN_BASE_ADDR_1_24_29_MSB	0x820

#define FLD_PL_IN_BASE_ADDR_2_0_7_MSB	0x824
#define FLD_PL_IN_BASE_ADDR_2_8_15_MSB	0x828
#define FLD_PL_IN_BASE_ADDR_2_16_23_MSB	0x82C
#define FLD_PL_IN_BASE_ADDR_2_24_29_MSB	0x830

#define FLD_PL_IN_BASE_ADDR_3_0_7_MSB	0x834
#define FLD_PL_IN_BASE_ADDR_3_8_15_MSB	0x838
#define FLD_PL_IN_BASE_ADDR_3_16_23_MSB	0x83C
#define FLD_PL_IN_BASE_ADDR_3_24_29_MSB	0x840

#define FLD_SH_IN_BASE_ADDR_0_7_MSB	0x844
#define FLD_SH_IN_BASE_ADDR_8_15_MSB	0x848
#define FLD_SH_IN_BASE_ADDR_16_23_MSB	0x84C
#define FLD_SH_IN_BASE_ADDR_24_29_MSB	0x850

#define FLD_BS_IN_BASE_ADDR_0_7_MSB	0x8d4
#define FLD_BS_IN_BASE_ADDR_8_15_MSB	0x8d8

#define FLD_TR_OUT_BASE_ADDR_0_MSB	0x854
#define FLD_PP_OUT_BASE_ADDR_0_MSB	0x858

#define FLD_BS_IN_BASE_ADDR_14		0x894

#define FLD_BS_BIAS			0x8E4
#define FLD_CV_FM_RANGE_0		0x8E8
#define FLD_CV_FM_RANGE_1		0x8EC
#define FLD_CV_PM_RANGE_0		0x8F0
#define FLD_CV_PM_RANGE_1		0x8F4
#define FLD_BS_RANGE_0			0x8F8
#define FLD_BS_RANGE_1			0x8FC

#define MTK_FD_OUTPUT_MIN_WIDTH		16U
#define MTK_FD_OUTPUT_MIN_HEIGHT	16U
#define MTK_FD_OUTPUT_MAX_WIDTH		4096U
#define MTK_FD_OUTPUT_MAX_HEIGHT	4096U

#define MTK_FD_HW_TIMEOUT_IN_MSEC	2000
#define RLT_NUM				48
#define GENDER_OUT			32

#define ETHNICITY_RST_X_NUM		4
#define ETHNICITY_RST_Y_NUM		64
#define GENDER_RST_X_NUM		2
#define GENDER_RST_Y_NUM		64
#define METHNICITY_RST_NUM		4
#define MGENDER_RST_NUM			2
#define MAGE_RST_NUM			2
#define MINDIAN_RST_NUM			2

#define FLD_FOREST			14
#define FLD_POINT			500

#define FLD_STEP_NUM			6
#define FLD_MAX_FRAME			15

#define FLD_STEP_BLINK			0
#define FLD_STEP_CV			1
#define FLD_STEP_FP			2
#define FLD_STEP_LEAF			3
#define FLD_STEP_KM02			4
#define FLD_STEP_KM13			5

#define FLD_BLINK_WEIGHT_FOREST14_SIZE	6416
#define FLD_CV_SIZE			19392
#define FLD_FP_SIZE			80160
#define FLD_LEAFNODE_SIZE		4608000
#define FLD_TREE_KM02_SIZE		120000
#define FLD_TREE_KM13_SIZE		120000
#define FLD_OUTPUT_SIZE			112

/* FLD_OUTPUT_X_SIZE: min: 1, max: 15 */
#define FLD_OUTPUT_X_SIZE		9

#define RESULT_SIZE			(RLT_NUM * MAX_FACE_NUM)

#define SRZ_BIT		(BIT_MASK(16) | BIT_MASK(12) | BIT_MASK(8) | BIT_MASK(0))
#define RESET_BIT16			BIT(16)
#define RESET_BIT17			BIT(17)
#define RESET_BIT			(RESET_BIT16 | RESET_BIT17)

#define ANCHOR_EN_NUM			5

#define FD_STRIDE_SIZE			3
#define FD_MAXPOOL_SIZE			3

#define ATTR_FD_STRIDE(n)		((n) == 0 ? 2 : 1)
#define ATTR_FD_MAXPOOL(n)		((n) == 0 ? 1 : 0)
#define FLD_FACE_INFO(m, n)		(fld_face_info_0[n] + 4 * (m))

enum Y2R_CONFIG {
	Y2R_SRC_DST_FORMAT,
	Y2R_IN_W_H,
	Y2R_OUT_W_H,
	Y2R_RA0_RA1_EN,
	Y2R_IN_X_Y_SIZE0,
	Y2R_IN_STRIDE0_BUS_SIZE0,
	Y2R_IN_X_Y_SIZE1,
	Y2R_IN_STRIDE1_BUS_SIZE1,
	Y2R_OUT_X_Y_SIZE0,
	Y2R_OUT_STRIDE0_BUS_SIZE0,
	Y2R_OUT_X_Y_SIZE1,
	Y2R_OUT_STRIDE1_BUS_SIZE1,
	Y2R_OUT_X_Y_SIZE2,
	Y2R_OUT_STRIDE2_BUS_SIZE2,
	Y2R_IN_0,
	Y2R_IN_1,
	Y2R_OUT_0,
	Y2R_OUT_1,
	Y2R_OUT_2,
	Y2R_RS_SEL_SRZ_EN,
	Y2R_X_Y_MAG,
	y2r_reserve0,
	Y2R_SRZ_HORI_STEP,
	Y2R_SRZ_VERT_STEP,
	y2r_reserve1,
	y2r_reserve2,
	Y2R_PADDING_EN_UP_DOWN,
	Y2R_PADDING_RIGHT_LEFT,
	Y2R_CO2_FMT_MODE_EN,
	Y2R_CO2_CROP_X,
	Y2R_CO2_CROP_Y,
	Y2R_CON_IN_BA_MSB,
	Y2R_CON_OUT_BA_MSB
};

enum RS_CONFIG {
	RS_X_Y_MAG = 1,
	RS_SRZ_HORI_STEP = 3,
	RS_SRZ_VERT_STEP,
	RS_INPUT_W_H = 7,
	RS_OUTPUT_W_H,
	RS_IN_X_Y_SIZE0 = 10,
	RS_IN_STRIDE0,
	RS_IN_X_Y_SIZE1,
	RS_IN_STRIDE1,
	RS_IN_X_Y_SIZE2,
	RS_IN_STRIDE2,
	RS_OUT_X_Y_SIZE0,
	RS_OUT_STRIDE0,
	RS_OUT_X_Y_SIZE1,
	RS_OUT_STRIDE1,
	RS_OUT_X_Y_SIZE2,
	RS_OUT_STRIDE2,
	RS_IN_0,
	RS_IN_1,
	RS_IN_2,
	RS_OUT_0,
	RS_OUT_1,
	RS_OUT_2,
	RS_CON_IN_BA_MSB,
	RS_CON_OUT_BA_MSB,
};

enum FD_CONFIG {
	FD_INPUT_ROTATE = 1,
	FD_CONV_WIDTH_MOD6,
	FD_CONV_IMG_W_H = 4,
	FD_IN_IMG_W_H,
	FD_OUT_IMG_W_H,
	FD_IN_X_Y_SIZE0 = 9,
	FD_IN_STRIDE0_BUS_SIZE0,
	FD_IN_X_Y_SIZE1,
	FD_IN_STRIDE1_BUS_SIZE1,
	FD_IN_X_Y_SIZE2,
	FD_IN_STRIDE2_BUS_SIZE2,
	FD_IN_X_Y_SIZE3,
	FD_IN_STRIDE3_BUS_SIZE3,
	FD_OUT_X_Y_SIZE0,
	FD_OUT_STRIDE0_BUS_SIZE0,
	FD_OUT_X_Y_SIZE1,
	FD_OUT_STRIDE1_BUS_SIZE1,
	FD_OUT_X_Y_SIZE2,
	FD_OUT_STRIDE2_BUS_SIZE2,
	FD_OUT_X_Y_SIZE3,
	FD_OUT_STRIDE3_BUS_SIZE3,
	FD_IN_0 = 27,
	FD_IN_1,
	FD_IN_2,
	FD_IN_3,
	FD_OUT_0,
	FD_OUT_1,
	FD_OUT_2,
	FD_OUT_3,
	FD_KERNEL_0,
	FD_KERNEL_1,
	FD_RPN_SET,
	FD_IMAGE_COORD,
	FD_IMAGE_COORD_XY_OFST,
	FD_BIAS_ACCU = 47,
	FD_SRZ_FDRZ_RS,
	FD_SRZ_HORI_STEP,
	FD_SRZ_VERT_STEP,
	FD_SRZ_HORI_SUB_INT_OFST,
	FD_SRZ_VERT_SUB_INT_OFST,
	FD_CON_IN_BA_MSB,
	FD_CON_OUT_BA_MSB,
	FD_CON_KERNEL_BA_MSB
};

static const unsigned int out_stride_size[FD_LOOP_NUM][OUTPUT_WDMA_WRA_NUM] = {
	{ 1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 1, 0, 2, 0 }, { 1, 0, 0, 0 },
	{ 1, 1, 2, 2 }, { 1, 1, 2, 2 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 },
	{ 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 1, 1, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 },
	{ 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 },
	{ 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 },
	{ 3, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 1, 0, 2, 0 },
	{ 1, 0, 0, 0 }, { 1, 1, 2, 2 }, { 1, 1, 2, 2 }, { 1, 0, 0, 0 },
	{ 1, 0, 2, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 },
	{ 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 1, 0, 0 },
	{ 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 1, 1, 1 },
	{ 1, 1, 1, 1 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 },
	{ 1, 0, 0, 0 }, { 3, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 },
	{ 1, 0, 2, 0 }, { 1, 0, 0, 0 }, { 1, 1, 2, 2 }, { 1, 1, 2, 2 },
	{ 1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 2, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 0, 0 }, { 1, 1, 0, 0 },
	{ 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 3, 0, 0, 0 }
};

static const unsigned int fd_ker_rdma_size[RPN_LOOP_NUM] = {
	240, 1168, 1168, 272,
	2320, 2080, 1040, 4624,
	3104, 9232, 4624, 4128,
	1040, 4624, 4624, 1552,
	4624, 4624, 4128, 1040,
	1040, 528, 4160, 4160,
	2080, 2080, 2080, 1040,
	0
};

static const unsigned int fd_out_stride2_in[FD_LOOP_NUM] = {
	0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,
	0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned int fd_stride_indices[FD_STRIDE_SIZE] = {
	0, 29, 58
};

static const unsigned int fd_maxpool_indices[FD_MAXPOOL_SIZE] = {
	1, 30, 59
};

static const unsigned int out_2size[FD_LOOP_NUM] = {
	0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 1,
	0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned int in_ch_pack[FD_LOOP_NUM] = {
	1,  16, 16, 16, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 0, 1, 16, 16, 16, 16, 16, 32,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	32, 32, 32, 0, 1, 16, 16, 16, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 0
};

static const unsigned int outlayer[FD_LOOP_NUM] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0
};

static const unsigned int out_ch_pack[FD_LOOP_NUM] = {
	16, 16, 16, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	32, 16, 16, 16, 32, 32, 32, 32, 32, 32, 0, 16, 16, 16, 16, 16, 32, 32,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 16, 16, 16, 32, 32, 32,
	32, 32, 32, 0, 16, 16, 16, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	32, 32, 32, 32, 32, 16, 16, 16, 32, 32, 32, 32, 32, 32, 0
};

static const signed int fd_rdma_en[FD_LOOP_NUM][INPUT_WDMA_WRA_NUM][2] = {
	{ { 99, 99 }, { 99, 99 }, { 99, 99 }, { -1, -1 } },
	{ { 0, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 1, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 1, 0 }, { 2, 0 }, { -1, -1 }, { -1, -1 } },
	{ { 3, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 1, 2 }, { 2, 2 }, { 4, 2 }, { 4, 3 } },
	{ { 5, 0 }, { 5, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 6, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 5, 0 }, { 5, 1 }, { 7, 0 }, { -1, -1 } },
	{ { 8, 0 }, { 8, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 9, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 5, 2 }, { 5, 3 }, { 7, 2 }, { 10, 2 } },
	{ { 11, 0 }, { 11, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 12, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 13, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 11, 0 }, { 11, 1 }, { 14, 0 }, { -1, -1 } },
	{ { 15, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 16, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 11, 0 }, { 11, 1 }, { 14, 0 }, { 17, 0 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { 18, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 19, 0 }, { 22, 0 }, { 22, 1 }, { 25, 0 } },
	{ { 99, 99 }, { 99, 99 }, { 99, 99 }, { -1, -1 } },
	{ { 29, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 30, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 30, 0 }, { 31, 0 }, { -1, -1 }, { -1, -1 } },
	{ { 32, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 30, 2 }, { 31, 2 }, { 33, 2 }, { 33, 3 } },
	{ { 34, 0 }, { 34, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 35, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 34, 0 }, { 34, 1 }, { 36, 0 }, { -1, -1 } },
	{ { 37, 0 }, { 37, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 38, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 34, 2 }, { 34, 3 }, { 36, 2 }, { 39, 2 } },
	{ { 40, 0 }, { 40, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 41, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 42, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 40, 0 }, { 40, 1 }, { 43, 0 }, { -1, -1 } },
	{ { 44, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 45, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 40, 0 }, { 40, 1 }, { 43, 0 }, { 46, 0 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 47, 0 }, { 47, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 48, 0 }, { 51, 0 }, { 51, 1 }, { 54, 0 } },
	{ { 99, 99 }, { 99, 99 }, { 99, 99 }, { -1, -1 } },
	{ { 58, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 59, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 59, 0 }, { 60, 0 }, { -1, -1 }, { -1, -1 } },
	{ { 61, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 59, 2 }, { 60, 2 }, { 62, 2 }, { 62, 3 } },
	{ { 63, 0 }, { 63, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 64, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 63, 0 }, { 63, 1 }, { 65, 0 }, { -1, -1 } },
	{ { 66, 0 }, { 66, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 67, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 63, 2 }, { 63, 3 }, { 65, 2 }, { 68, 2 } },
	{ { 69, 0 }, { 69, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 70, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 71, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 69, 0 }, { 69, 1 }, { 72, 0 }, { -1, -1 } },
	{ { 73, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 74, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 69, 0 }, { 69, 1 }, { 72, 0 }, { 75, 0 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 76, 0 }, { 76, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 77, 0 }, { 80, 0 }, { 80, 1 }, { 83, 0 } }
};

static const unsigned int attr_wdma_en[ATTR_LOOP_NUM][OUTPUT_WDMA_WRA_NUM] = {
	{ 1, 0, 1, 0 }, { 1, 0, 1, 0 }, { 1, 0, 0, 0 }, { 1, 1, 1, 1 },
	{ 1, 1, 1, 1 }, { 1, 0, 1, 0 }, { 1, 1, 0, 0 }, { 1, 0, 1, 0 },
	{ 1, 1, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 1, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 },
	{ 1, 0, 0, 0 }, { 1, 0, 0, 0 }
};

static const unsigned int attr_ker_rdma_size[ATTR_LOOP_NUM] = {
	240, 1168, 272, 2320,
	2080, 9232, 3104, 9232,
	4128, 1040, 4624, 4624,
	1552, 4624, 4624, 4128,
	9232, 272, 9232, 2320,
	144, 9232, 272, 9232,
	2320, 144
};

static const unsigned int attr_out_stride2_as_in[ATTR_LOOP_NUM] = {
	0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned int attr_out_2size[ATTR_LOOP_NUM] = { /* O */
	1, 1, 0, 1, 1, 1, 0,
	1, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0
};

/* [loop][ch][output_index] */
static const signed int attr_rdma_en[ATTR_LOOP_NUM][INPUT_WDMA_WRA_NUM][2] = {
	{ { 99, 99 }, { 99, 99 }, { 99, 99 }, { -1, -1 } },
	{ { 0, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 0, 0 }, { 1, 0 }, { -1, -1 }, { -1, -1 } },
	{ { 2, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 0, 2 }, { 1, 2 }, { 3, 2 }, { 3, 3 } },
	{ { 4, 0 }, { 4, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 4, 0 }, { 4, 1 }, { 5, 0 }, { -1, -1 } },
	{ { 6, 0 }, { 6, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 4, 2 }, { 4, 3 }, { 5, 2 }, { 7, 2 } },
	{ { 8, 0 }, { 8, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 9, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 10, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 8, 0 }, { 8, 1 }, { 11, 0 }, { -1, -1 } },
	{ { 12, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 13, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 8, 0 }, { 8, 1 }, { 11, 0 }, { 14, 0 } },
	{ { 15, 0 }, { 15, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 16, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 15, 0 }, { 15, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 18, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 19, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 15, 0 }, { 15, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 21, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 15, 0 }, { 15, 1 }, { -1, -1 }, { -1, -1 } },
	{ { 23, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
	{ { 24, 0 }, { -1, -1 }, { -1, -1 }, { -1, -1 } }
};

static const unsigned int attr_wdma_size[ATTR_LOOP_NUM][OUTPUT_WDMA_WRA_NUM] = {
	{ 16384, 0, 4096, 0 },
	{ 16384, 0, 4096, 0 },
	{ 16384, 0, 0, 0 },
	{ 16384, 16384, 4096, 4096 },
	{ 8192, 8192, 2048, 2048 },
	{ 8192, 0, 2048, 0 },
	{ 8192, 8192, 0, 0 },
	{ 8192, 0, 2048, 0 },
	{ 2048, 2048, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 2048, 2048, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 1024, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 0, 0, 0, 0 },
	{ 2048, 0, 0, 0 },
	{ 1024, 0, 0, 0 },
	{ 0, 0, 0, 0 }
};

static const unsigned int fld_face_info_0[FLD_MAX_FRAME] = {
	0x440, 0x44C, 0x458, 0x464, 0x470, 0x47C, 0x488, 0x494, 0x4A4,
	0x4B0, 0x4BC, 0x4C8, 0x4D4, 0x4E0, 0x4EC
};

struct aie_static_info_element {
	u32 fd_wdma_size[OUTPUT_WDMA_WRA_NUM];
	u32 out_xsize_plus_1;
	u32 out_height;
	u32 out_ysize_plus_1_stride2;
	u32 out_stride;
	u32 out_stride_stride2;
	u32 out_width;
	u32 img_width;
	u32 img_height;
	u32 stride2_out_width;
	u32 stride2_out_height;
	u32 out_xsize_plus_1_stride2;
	u32 input_xsize_plus_1;
};

struct aie_static_info {
	struct aie_static_info_element inf_elm[FD_LOOP_NUM];
};

enum aie_state {
	STATE_NA,
	STATE_INIT,
	STATE_OPEN
};

/**
 * enum aie_mode - AIE Feature Mode
 * @FDMODE: Face Detection.
 * @ATTRIBUTEMODE: Gender and ethnicity detection.
 * @FLDMODE: Locations of eyebrows, eyes, ears, nose,and mouth.
 */
enum aie_mode {
	FDMODE,
	ATTRIBUTEMODE,
	FLDMODE
};

enum aie_format {
	FMT_NA,
	FMT_YUV_2P,
	FMT_YVU_2P,
	FMT_YUYV,
	FMT_YVYU,
	FMT_UYVY,
	FMT_VYUY,
	FMT_MONO,
	FMT_YUV420_2P,
	FMT_YUV420_1P
};

enum aie_input_degree {
	DEGREE_0,
	DEGREE_90,
	DEGREE_270,
	DEGREE_180
};

struct ethnicity_result {
	signed short result[4][64];
};

struct merged_ethnicity_result {
	signed short result[4];
};

struct merged_gender_result {
	signed short result[2];
};

struct merged_age_result {
	signed short result[2];
};

struct merged_is_indian_result {
	signed short result[2];
};

struct aie_reg_cfg {
	u32 rs_adr;
	u32 yuv2rgb_adr;
	u32 fd_adr;
	u32 fd_pose_adr;
	u32 fd_mode;
	u32 hw_result;
	u32 hw_result1;
	u32 reserved;
};

struct aie_hw_rect {
	u16 width;
	u16 height;
};

struct aie_para {
	void *fd_fd_cfg_va;
	void *fd_rs_cfg_va;
	void *fd_yuv2rgb_cfg_va;

	void *attr_fd_cfg_va[MAX_ENQUE_FRAME_NUM];
	void *attr_yuv2rgb_cfg_va[MAX_ENQUE_FRAME_NUM];

	void *rs_pym_rst_va[PYM_NUM][COLOR_NUM];

	dma_addr_t fd_fd_cfg_pa;
	dma_addr_t fd_rs_cfg_pa;
	dma_addr_t fd_yuv2rgb_cfg_pa;

	dma_addr_t attr_fd_cfg_pa[MAX_ENQUE_FRAME_NUM];
	dma_addr_t attr_yuv2rgb_cfg_pa[MAX_ENQUE_FRAME_NUM];

	dma_addr_t rs_pym_rst_pa[PYM_NUM][COLOR_NUM];

	u32 sel_mode;
	u32 src_img_fmt;
	u32 rotate_degree;
	s16 rpn_anchor_thrd;
	u16 number_of_pyramid;
	u32 src_img_addr;
	u32 src_img_addr_uv;

	struct aie_hw_rect max_img_rect;
	struct aie_hw_rect img_rect;
	struct aie_hw_rect crop_rect;
	struct aie_hw_rect pyramid_rect;
	struct aie_hw_rect max_pyramid_rect;
};

struct aie_attr_para {
	u32 w_idx;
	u32 r_idx;
	u32 sel_mode[MAX_ENQUE_FRAME_NUM];
	u16 img_width[MAX_ENQUE_FRAME_NUM];
	u16 img_height[MAX_ENQUE_FRAME_NUM];
	u16 crop_width[MAX_ENQUE_FRAME_NUM];
	u16 crop_height[MAX_ENQUE_FRAME_NUM];
	u32 src_img_fmt[MAX_ENQUE_FRAME_NUM];
	u32 rotate_degree[MAX_ENQUE_FRAME_NUM];
	u32 src_img_addr[MAX_ENQUE_FRAME_NUM];
	u32 src_img_addr_uv[MAX_ENQUE_FRAME_NUM];
};

struct aie_fd_dma_para {
	void *fd_out_hw_va[FD_LOOP_NUM][OUTPUT_WDMA_WRA_NUM];
	void *fd_kernel_va[FD_LOOP_NUM][KERNEL_RDMA_RA_NUM];
	void *attr_out_hw_va[ATTR_LOOP_NUM][OUTPUT_WDMA_WRA_NUM];
	void *attr_kernel_va[ATTR_LOOP_NUM][KERNEL_RDMA_RA_NUM];

	void *age_out_hw_va[MAX_ENQUE_FRAME_NUM];
	void *gender_out_hw_va[MAX_ENQUE_FRAME_NUM];
	void *is_indian_out_hw_va[MAX_ENQUE_FRAME_NUM];
	void *ethnicity_out_hw_va[MAX_ENQUE_FRAME_NUM];

	dma_addr_t fd_out_hw_pa[FD_LOOP_NUM][OUTPUT_WDMA_WRA_NUM];
	dma_addr_t fd_kernel_pa[FD_LOOP_NUM][KERNEL_RDMA_RA_NUM];
	dma_addr_t attr_out_hw_pa[ATTR_LOOP_NUM][OUTPUT_WDMA_WRA_NUM];
	dma_addr_t attr_kernel_pa[ATTR_LOOP_NUM][KERNEL_RDMA_RA_NUM];

	dma_addr_t age_out_hw_pa[MAX_ENQUE_FRAME_NUM];
	dma_addr_t gender_out_hw_pa[MAX_ENQUE_FRAME_NUM];
	dma_addr_t is_indian_out_hw_pa[MAX_ENQUE_FRAME_NUM];
	dma_addr_t ethnicity_out_hw_pa[MAX_ENQUE_FRAME_NUM];
};

struct aie_fd_fld_para {
	void *fld_step_va[FLD_STEP_NUM][FLD_MAX_FRAME];
	void *fld_output_va[FLD_MAX_FRAME];
	dma_addr_t fld_step_pa[FLD_STEP_NUM][FLD_MAX_FRAME];
	dma_addr_t fld_output_pa[FLD_MAX_FRAME];
};

struct imem_buf_info {
	void *va;
	dma_addr_t pa;
	unsigned int size;
	unsigned int reserved;
};

struct fd_buffer {
	/* used by DMA HW */
	u32 dma_addr;
};

struct aie_clocks {
	struct clk_bulk_data *clks;
	unsigned int clk_num;
};

struct mtk_aie_req_work {
	struct work_struct work;
	struct mtk_aie_dev *fd_dev;
};

struct mtk_aie_variant {
	unsigned int y2r_cfg_size;
	unsigned int rs_cfg_size;
	unsigned int fd_cfg_size;
};

struct mtk_aie_dev {
	struct device *dev;
	struct mtk_aie_ctx *ctx;
	struct v4l2_m2m_dev *m2m_dev;
	struct device *larb;
	struct aie_para *base_para;
	struct aie_attr_para *attr_para;
	struct aie_fd_dma_para *dma_para;

	struct aie_fd_fld_para *fld_para;

	struct aie_enq_info *aie_cfg;
	struct workqueue_struct *frame_done_wq;
	void __iomem *fd_base;
	const struct mtk_aie_variant *variant;

	/* Input Buffer Pointer */
	struct imem_buf_info rs_cfg_data;
	struct imem_buf_info fd_cfg_data;
	struct imem_buf_info yuv2rgb_cfg_data;
	/* HW Output Buffer Pointer */
	struct imem_buf_info rs_output_hw;
	struct imem_buf_info fd_dma_hw;
	struct imem_buf_info fd_dma_result_hw;
	struct imem_buf_info fd_kernel_hw;
	struct imem_buf_info fd_attr_dma_hw;
	struct aie_static_info st_info;

	struct aie_reg_cfg reg_cfg;

	/* Fld fw buffer */
	struct media_device mdev;
	struct video_device vfd;
	struct aie_clocks aie_clk;
	struct v4l2_device v4l2_dev;

	/* Lock for V4L2 operations */
	struct mutex vfd_lock;
	/* Lock for device operations */
	struct mutex dev_lock;
	/* Lock for performance optimization */
	struct mutex fd_lock;
	struct imem_buf_info fd_fld_step_data;
	struct imem_buf_info fd_fld_out_hw;

	int irq;
	struct completion fd_job_finished;
	struct delayed_work job_timeout_work;

	/* DRAM Buffer Size */
	unsigned int fd_rs_cfg_size;
	unsigned int fd_fd_cfg_size;
	unsigned int fd_yuv2rgb_cfg_size;
	unsigned int attr_fd_cfg_size;
	unsigned int attr_yuv2rgb_cfg_size;

	/* HW Output Buffer Size */
	unsigned int rs_pym_out_size[PYM_NUM];
	unsigned int fd_dma_max_size;
	unsigned int fd_dma_rst_max_size;
	unsigned int fd_fd_kernel_size;
	unsigned int fd_attr_kernel_size;
	unsigned int fd_attr_dma_max_size;
	unsigned int fd_attr_dma_rst_max_size;

	/* Fld size */
	unsigned int fld_step_size;
	unsigned int fld_out_size;

	wait_queue_head_t flushing_waitq;
	atomic_t num_composing;
	struct mtk_aie_req_work req_work;
	unsigned int fd_state;
	unsigned int fd_mem_size;
	u32 fd_stream_count;
};

struct mtk_aie_ctx {
	struct mtk_aie_dev *fd_dev;
	struct device *dev;
	struct v4l2_fh fh;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_meta_format dst_fmt;
	struct v4l2_ctrl_aie_init user_init;
	struct v4l2_ctrl_aie_param user_param;
};

void aie_reset(struct mtk_aie_dev *fd);
int aie_init(struct mtk_aie_dev *fd, struct v4l2_ctrl_aie_init *user_init);
void aie_uninit(struct mtk_aie_dev *fd);
void aie_prepare(struct mtk_aie_dev *fd, struct aie_enq_info *aie_cfg);
void aie_execute(struct mtk_aie_dev *fd, struct aie_enq_info *aie_cfg);
void aie_irqhandle(struct mtk_aie_dev *fd);
void aie_get_fd_result(struct mtk_aie_dev *fd, struct aie_enq_info *aie_cfg);
void aie_get_attr_result(struct mtk_aie_dev *fd, struct aie_enq_info *aie_cfg);
void aie_get_fld_result(struct mtk_aie_dev *fd, struct aie_enq_info *aie_cfg);
#endif /*__MTK_AIE_H__*/
