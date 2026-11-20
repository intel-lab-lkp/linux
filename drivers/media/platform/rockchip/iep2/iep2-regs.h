/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
/*
 * Based on the Rockchip BSP rockchip_iep2_regs.h
 * Copyright (c) 2020 Fuzhou Rockchip Electronics Co., Ltd
 */

#ifndef __ROCKCHIP_IEP2_REGS_H__
#define __ROCKCHIP_IEP2_REGS_H__

#include <linux/bits.h>

#define IEP2_FRM_START			0x0000
#define   IEP2_FRM_EN			BIT(0)

#define IEP2_CONFIG0			0x0004
#define   IEP2_CCLK_SRESET_P		BIT(22)
#define   IEP2_ACLK_SRESET_P		BIT(21)
#define   IEP2_HANDSAVE_P		BIT(20)
#define   IEP2_RST_PROTECT_EN		BIT(19)
#define   IEP2_DEBUG_DATA_EN		BIT(16)
#define   IEP2_DST_YUV_SWAP(x)		(((x) & 3) << 12)
#define   IEP2_DST_FMT(x)		(((x) & 3) << 8)
#define   IEP2_SRC_YUV_SWAP(x)		(((x) & 3) << 4)
#define   IEP2_SRC_FMT(x)		((x) & 3)

#define IEP2_FMT_YUV422			2
#define IEP2_FMT_YUV420			3

#define IEP2_YUV_SWAP_SP_UV		0
#define IEP2_YUV_SWAP_SP_VU		1

#define IEP2_WORK_MODE			0x0008
#define   IEP2_WORK_MODE_IEP2		BIT(0)
#define   IEP2_WORK_MODE_MASK		GENMASK(1, 0)

#define IEP2_GATING_CTRL		0x0010

#define IEP2_STATUS			0x0014
#define   IEP2_ARST_FINISH_DONE		BIT(0)

#define IEP2_INT_EN			0x0020
#define IEP2_INT_CLR			0x0024
#define IEP2_INT_STS			0x0028
#define IEP2_INT_RAW_STS		0x002c
#define   IEP2_INT_TIMEOUT		BIT(5)
#define   IEP2_INT_BUS_ERROR		BIT(4)
#define   IEP2_INT_OSD_MAX		BIT(1)
#define   IEP2_INT_FRM_DONE		BIT(0)
#define   IEP2_INT_VALID		(IEP2_INT_TIMEOUT | IEP2_INT_BUS_ERROR | \
					 IEP2_INT_FRM_DONE)

/* Strides are in units of 32-bit words */
#define IEP2_VIR_SRC_IMG_WIDTH		0x0030
#define   IEP2_SRC_VIR_UV_STRIDE(x)	(((x) & 0xffff) << 16)
#define   IEP2_SRC_VIR_Y_STRIDE(x)	((x) & 0xffff)
#define IEP2_VIR_DST_IMG_WIDTH		0x0034
#define   IEP2_DST_VIR_STRIDE(x)	((x) & 0xffff)

#define IEP2_SRC_IMG_SIZE		0x0038
#define   IEP2_SRC_PIC_HEIGHT(x)	(((x) & 0x7ff) << 16)
#define   IEP2_SRC_PIC_WIDTH(x)		((x) & 0x7ff)

#define IEP2_DIL_CONFIG0		0x0040
#define   IEP2_DIL_MV_HIST_EN		BIT(17)
#define   IEP2_DIL_ROI_EN		BIT(16)
#define   IEP2_DIL_COMB_EN		BIT(15)
#define   IEP2_DIL_BLE_EN		BIT(14)
#define   IEP2_DIL_EEDI_EN		BIT(13)
#define   IEP2_DIL_MEMC_EN		BIT(12)
#define   IEP2_DIL_OSD_EN		BIT(11)
#define   IEP2_DIL_PD_EN		BIT(10)
#define   IEP2_DIL_FF_EN		BIT(9)
#define   IEP2_DIL_MD_PRE_EN		BIT(8)
#define   IEP2_DIL_FIELD_ORDER(x)	(((x) & 1) << 5)
#define   IEP2_DIL_OUT_MODE(x)		(((x) & 1) << 4)
#define   IEP2_DIL_MODE(x)		((x) & 0xf)

#define IEP2_DIL_FIELD_ORDER_TB		0
#define IEP2_DIL_FIELD_ORDER_BT		1

#define IEP2_DIL_OUT_MODE_LINE		0
#define IEP2_DIL_OUT_MODE_TILE		1

#define IEP2_DIL_MODE_DISABLE		0
#define IEP2_DIL_MODE_I5O2		1
#define IEP2_DIL_MODE_I5O1T		2
#define IEP2_DIL_MODE_I5O1B		3
#define IEP2_DIL_MODE_I2O2		4
#define IEP2_DIL_MODE_I1O1T		5
#define IEP2_DIL_MODE_I1O1B		6
#define IEP2_DIL_MODE_PD		7
#define IEP2_DIL_MODE_BYPASS		8
#define IEP2_DIL_MODE_DECT		9

#define IEP2_TIMEOUT_CFG		0x0050
#define   IEP2_TIMEOUT_CFG_EN		BIT(31)
#define   IEP2_TIMEOUT_CFG_MAX		GENMASK(25, 0)

#define IEP2_DBG_FRM_CNT		0x0058
#define IEP2_DBG_TIMEOUT_CNT		0x005c

#define IEP2_SRC_ADDR_CURY		0x0060
#define IEP2_SRC_ADDR_NXTY		0x0064
#define IEP2_SRC_ADDR_PREY		0x0068
#define IEP2_SRC_ADDR_CURUV		0x006c
#define IEP2_SRC_ADDR_CURV		0x0070
#define IEP2_SRC_ADDR_NXTUV		0x0074
#define IEP2_SRC_ADDR_NXTV		0x0078
#define IEP2_SRC_ADDR_PREUV		0x007c
#define IEP2_SRC_ADDR_PREV		0x0080
#define IEP2_SRC_ADDR_MD		0x0084
#define IEP2_SRC_ADDR_MV		0x0088
#define IEP2_ROI_ADDR			0x008c
#define IEP2_DST_ADDR_TOPY		0x00b0
#define IEP2_DST_ADDR_BOTY		0x00b4
#define IEP2_DST_ADDR_TOPC		0x00b8
#define IEP2_DST_ADDR_BOTC		0x00bc
#define IEP2_DST_ADDR_MD		0x00c0
#define IEP2_DST_ADDR_MV		0x00c4

#define IEP2_MD_CONFIG0			0x00e0
#define   IEP2_MD_THETA(x)		(((x) & 3) << 8)
#define   IEP2_MD_R(x)			(((x) & 0xf) << 4)
#define   IEP2_MD_LAMBDA(x)		((x) & 0xf)

#define IEP2_DECT_CONFIG0		0x00e4
#define   IEP2_OSD_GRADV_THR(x)		(((x) & 0xff) << 24)
#define   IEP2_OSD_GRADH_THR(x)		(((x) & 0xff) << 16)
#define   IEP2_OSD_AREA_NUM(x)		(((x) & 0xf) << 8)
#define   IEP2_DECT_RESI_THR(x)		((x) & 0xff)

#define IEP2_OSD_LIMIT_CONFIG		0x00f0
#define   IEP2_OSD_POS_LIMIT_NUM(x)	(((x) & 7) << 4)
#define   IEP2_OSD_POS_LIMIT_EN		BIT(0)
#define IEP2_OSD_LIMIT_AREA(i)		(0x00f4 + ((i) * 4))

#define IEP2_OSD_CONFIG0		0x00fc
#define   IEP2_OSD_LINE_NUM(x)		(((x) & 0x1ff) << 16)
#define   IEP2_OSD_PEC_THR(x)		((x) & 0x7ff)

#define IEP2_OSD_AREA_CONF(i)		(0x0100 + ((i) * 4))
#define IEP2_OSD_AREA_NUM_MAX		8

#define IEP2_ME_CONFIG0			0x0120
#define   IEP2_ME_THR_OFFSET(x)		(((x) & 0xff) << 16)
#define   IEP2_MV_SIMILAR_NUM_THR0(x)	(((x) & 0xf) << 12)
#define   IEP2_MV_SIMILAR_THR(x)	(((x) & 0xf) << 8)
#define   IEP2_MV_BONUS(x)		(((x) & 0xf) << 4)
#define   IEP2_ME_PENA(x)		((x) & 0xf)

#define IEP2_ME_LIMIT_CONFIG		0x0124
#define   IEP2_MV_RIGHT_LIMIT(x)	(((x) & 0x3f) << 8)
#define   IEP2_MV_LEFT_LIMIT(x)		((x) & 0x3f)

#define IEP2_MV_TRU_LIST(i)		(0x0128 + ((i) * 4))
#define IEP2_MV_TRU_LIST_NUM		2

#define IEP2_EEDI_CONFIG0		0x0130
#define   IEP2_EEDI_THR0(x)		((x) & 0x1f)

#define IEP2_BLE_CONFIG0		0x0134
#define   IEP2_BLE_BACKTOMA_NUM(x)	((x) & 7)

#define IEP2_COMB_CONFIG0		0x0138
#define   IEP2_COMB_CNT_THR(x)		(((x) & 0xf) << 24)
#define   IEP2_COMB_FEATURE_THR(x)	(((x) & 0x3f) << 16)
#define   IEP2_COMB_T_THR(x)		(((x) & 0xff) << 8)
#define   IEP2_COMB_OSD_VLD(x)		((x) & 0xff)

#define IEP2_DIL_MTN_TAB(i)		(0x0140 + ((i) * 4))
#define IEP2_DIL_MTN_TAB_NUM		16

#define IEP2_RO_PD_TCNT			0x0400
#define IEP2_RO_PD_BCNT			0x0404
#define IEP2_RO_FF_CUR_TCNT		0x0408
#define IEP2_RO_FF_CUR_BCNT		0x040c
#define IEP2_RO_FF_NXT_TCNT		0x0410
#define IEP2_RO_FF_NXT_BCNT		0x0414
#define IEP2_RO_FF_BLE_TCNT		0x0418
#define IEP2_RO_FF_BLE_BCNT		0x041c
#define IEP2_RO_FF_COMB_NZ		0x0420
#define IEP2_RO_FF_COMB_F		0x0424
#define IEP2_RO_OSD_NUM			0x0428
#define IEP2_RO_COMB_CNT		0x042c
#define IEP2_RO_FF_GRADT_TCNT		0x0430
#define IEP2_RO_FF_GRADT_BCNT		0x0434
#define IEP2_RO_OSD_AREA_X(i)		(0x0440 + ((i) * 8))
#define IEP2_RO_OSD_AREA_Y(i)		(0x0444 + ((i) * 8))
#define IEP2_RO_MV_HIST_BIN(i)		(0x0480 + ((i) * 4))

#endif /* __ROCKCHIP_IEP2_REGS_H__ */
