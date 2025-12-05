/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_HW_ERROR_REGS_H_
#define _XE_HW_ERROR_REGS_H_

#define HEC_UNCORR_ERR_STATUS(base)                    XE_REG((base) + 0x118)
#define    UNCORR_FW_REPORTED_ERR                      BIT(6)

#define HEC_UNCORR_FW_ERR_DW0(base)                    XE_REG((base) + 0x124)

#define ERR_STAT_GT_COR				0x100160
#define ERR_STAT_GT_NONFATAL			0x100164
#define ERR_STAT_GT_FATAL			0x100168
#define ERR_STAT_GT_REG(x)			XE_REG(_PICK_EVEN((x), \
								 ERR_STAT_GT_COR, \
								 ERR_STAT_GT_NONFATAL))

#define  GT_HW_ERROR_MAX_ERR_BITS		16
#define  EU_GRF_ERR				(15)
#define  EU_IC_ERR				(14)
#define  SLM_ERR				(13)
#define  GUC_COR_ERR				(1)

#define  GUC_FAT_ERR				(6)
#define  FPU_FAT_ERR				(3)

#define PVC_COR_ERR_MASK			(BIT(GUC_COR_ERR) | BIT(SLM_ERR) | \
						 BIT(EU_IC_ERR) | BIT(EU_GRF_ERR))

#define PVC_FAT_ERR_MASK			(BIT(FPU_FAT_ERR) | BIT(GUC_FAT_ERR) | \
						 BIT(EU_GRF_ERR) | BIT(SLM_ERR))

#define DEV_ERR_STAT_NONFATAL			0x100178
#define DEV_ERR_STAT_CORRECTABLE		0x10017c
#define DEV_ERR_STAT_REG(x)			XE_REG(_PICK_EVEN((x), \
								  DEV_ERR_STAT_CORRECTABLE, \
								  DEV_ERR_STAT_NONFATAL))

#define   XE_CSC_ERROR				BIT(17)
#define   XE_GT_ERROR				BIT(0)

#define  ERR_STAT_GT_FATAL_VECTOR_0		0x100260
#define  ERR_STAT_GT_FATAL_VECTOR_1		0x100264

#define  ERR_STAT_GT_FATAL_VECTOR_REG(x)	XE_REG(_PICK_EVEN((x), \
								  ERR_STAT_GT_FATAL_VECTOR_0, \
								  ERR_STAT_GT_FATAL_VECTOR_1))

#define  ERR_STAT_GT_COR_VECTOR_LEN		(4)
#define  ERR_STAT_GT_COR_VECTOR_0		0x1002a0
#define  ERR_STAT_GT_COR_VECTOR_1		0x1002a4

#define  ERR_STAT_GT_COR_VECTOR_REG(x)		XE_REG(_PICK_EVEN((x), \
								 ERR_STAT_GT_COR_VECTOR_0,\
								 ERR_STAT_GT_COR_VECTOR_1))

#define ERR_STAT_GT_VECTOR_REG(hw_err, x)	(hw_err == DRM_XE_RAS_ERROR_CORRECTABLE ? \
						 ERR_STAT_GT_COR_VECTOR_REG(x) : \
						 ERR_STAT_GT_FATAL_VECTOR_REG(x))

#endif
