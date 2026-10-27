/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Renesas Electronics Corp.
 * Copyright (C) 2026 Ideas on Board Oy
 * Copyright (C) 2026 Ragnatech AB
 */

#ifndef __MEDIA_RPPX1_H__
#define __MEDIA_RPPX1_H__

#include <linux/device.h>
#include <linux/types.h>

#include "rpp_module.h"

/* RPP_HDR Base Addresses */
#define RPPX1_HDRREGS_BASE			0x0000
#define RPPX1_HDR_IRQ_BASE			0x0200
#define RPPX1_RPP_OUT_BASE			0x0800
#define RPPX1_RPP_RMAP_BASE			0x0c00
#define RPPX1_RPP_RMAP_MEAS_BASE		0x1000
#define RPPX1_RPP_MAIN_PRE1_BASE		0x2000
#define RPPX1_RPP_MAIN_PRE2_BASE		0x4000
#define RPPX1_RPP_MAIN_POST_BASE		0xa000
#define RPPX1_RPP_MVOUT_BASE			0xc000
#define RPPX1_RPP_FUSA_BASE			0xf000

#define RPPX1_RPP_HDRREGS_VERSION_REG          (RPPX1_HDRREGS_BASE + 0x0000)

#define RPPX1_RPP_HDR_UPD_REG                  (RPPX1_HDRREGS_BASE + 0x0004)
#define RPPX1_RPP_HDR_UPD_REGS_GEN_CFG_UPD     BIT(1)
#define RPPX1_RPP_HDR_UPD_REGS_CFG_UPD         BIT(0)

#define RPPX1_RESERVED_3_REG                   (RPPX1_HDRREGS_BASE + 0x0008)

#define RPPX1_RPP_HDR_INFORM_ENABLE_REG                (RPPX1_HDRREGS_BASE + 0x000c)
#define RPPX1_RPP_HDR_INFORM_ENABLE_ENABLE     1
#define RPPX1_RPP_HDR_INFORM_ENABLE_DISABLE    0

#define RPPX1_RPP_HDR_OUT_IF_ON_REG            (RPPX1_HDRREGS_BASE + 0x0010)
#define RPPX1_RPP_HDR_OUT_IF_OFF_REG           (RPPX1_HDRREGS_BASE + 0x0014)

#define RPPX1_RPP_HDR_SAFETY_ACCESS_PROTECTION_REG     (RPPX1_HDRREGS_BASE + 0x0018)
#define RPPX1_RPP_HDR_SAFETY_ACCESS_PROTECTION_ENABLE  1
#define RPPX1_RPP_HDR_SAFETY_ACCESS_PROTECTION_DISABLE 0

#define RPPX1_RPP_ISM                          (RPPX1_HDR_IRQ_BASE + 0x00)
#define RPPX1_RPP_RIS                          (RPPX1_HDR_IRQ_BASE + 0x04)
#define RPPX1_RPP_MIS                          (RPPX1_HDR_IRQ_BASE + 0x08)
#define RPPX1_RPP_ISC                          (RPPX1_HDR_IRQ_BASE + 0x0c)

/* RPP_OUT/MV_OUT Pipelines - Base Addresses */
#define RPPX1_GAMMA_OUT_BASE			0x0000 /* HV, MV */
#define RPPX1_IS_BASE				0x00c0 /* HV, MV */
#define RPPX1_CSM_BASE				0x0100 /* HV, MV */
#define RPPX1_OUT_IF_BASE			0x0200 /* HV, MV */
#define RPPX1_RPP_OUTREGS_BASE			0x02c0 /* HV, MV */
#define RPPX1_LUV_BASE				0x0300 /* MV */

/* PRE1/PRE2/POST Pipelines - Base Addresses */
#define RPPX1_ACQ_BASE				0x0080 /* PRE1, PRE2 */
#define RPPX1_BLS_BASE				0x0100 /* PRE1, PRE2 */
#define RPPX1_GAMMA_IN_BASE			0x0200 /* PRE1, PRE2 */
#define RPPX1_LSC_BASE				0x0400 /* PRE1, PRE2 */
#define RPPX1_AWB_GAIN_BASE			0x0500 /* PRE1, PRE2, POST */
#define RPPX1_DPCC_BASE				0x0600 /* PRE1, PRE2 */
#define RPPX1_DPF_BASE				0x0700 /* PRE1, PRE2 */
#define RPPX1_FILT_BASE				0x0800 /* POST */
#define RPPX1_CAC_BASE				0x0880 /* POST */
#define RPPX1_CCOR_BASE				0x0900 /* POST */
#define RPPX1_HIST_BASE				0x0a00 /* PRE1, PRE2, POST */
#define RPPX1_HIST256_BASE			0x0b00 /* PRE1 */
#define RPPX1_EXM_BASE				0x0c00 /* PRE1, PRE2 */
#define RPPX1_LTM_BASE				0x1000 /* POST */
#define RPPX1_LTM_MEAS_BASE			0x1200 /* POST */
#define RPPX1_WBMEAS_BASE			0x1700 /* POST */
#define RPPX1_BDRGB_BASE			0x1800 /* POST */
#define RPPX1_SHRP_BASE				0x1a00 /* POST */

/* Functional Safety Module Base Addresses */
#define RPPX1_FMU_BASE				0x0100

#define RPPX1_RPP_HDR_FMU_FSM_REG		(RPPX1_RPP_FUSA_BASE + RPPX1_FMU_BASE + 0x00)
#define RPPX1_RPP_HDR_FMU_FSM_FSM_IRQM_FAULT	BIT(23)
#define RPPX1_RPP_HDR_FMU_FSM_PRE2_SIZE_FAULT	BIT(20)
#define RPPX1_RPP_HDR_FMU_FSM_PRE2_TIME_FAULT	BIT(19)
#define RPPX1_RPP_HDR_FMU_FSM_PRE1_SIZE_FAULT	BIT(18)
#define RPPX1_RPP_HDR_FMU_FSM_PRE1_TIME_FAULT	BIT(17)
#define RPPX1_RPP_HDR_FMU_FSM_SIZE_FAULT	BIT(16)
#define RPPX1_RPP_HDR_FMU_FSM_TIME_FAULT	BIT(15)
#define RPPX1_RPP_HDR_FMU_FSM_MV_OUT_SIZE_FAULT	BIT(14)
#define RPPX1_RPP_HDR_FMU_FSM_MV_OUT_TIME_FAULT	BIT(13)
#define RPPX1_RPP_HDR_FMU_FSM_HV_OUT_SIZE_FAULT	BIT(12)
#define RPPX1_RPP_HDR_FMU_FSM_HV_OUT_TIME_FAULT	BIT(11)
#define RPPX1_RPP_HDR_FMU_FSM_MV_OUT_SIZE_ERR	BIT(10)
#define RPPX1_RPP_HDR_FMU_FSM_IS_OUT_SIZE_ERR	BIT(9)
#define RPPX1_RPP_HDR_FMU_FSM_PRE2_FIFO_OVFLW	BIT(7)
#define RPPX1_RPP_HDR_FMU_FSM_PRE1_FIFO_OVFLW	BIT(6)
#define RPPX1_RPP_HDR_FMU_FSM_PRE1_INFORM_SIZE	BIT(5)
#define RPPX1_RPP_HDR_FMU_FSM_PRE1_OUTFORM_SIZE	BIT(4)
#define RPPX1_RPP_HDR_FMU_FSM_PRE2_INFORM_SIZE	BIT(3)
#define RPPX1_RPP_HDR_FMU_FSM_PRE2_OUTFORM_SIZE	BIT(2)

#define RPPX1_RPP_HDR_FMU_RFS_REG		(RPPX1_RPP_FUSA_BASE + RPPX1_FMU_BASE + 0x04)
#define RPPX1_RPP_HDR_FMU_MFS_REG		(RPPX1_RPP_FUSA_BASE + RPPX1_FMU_BASE + 0x08)
#define RPPX1_RPP_HDR_FMU_FSC_REG		(RPPX1_RPP_FUSA_BASE + RPPX1_FMU_BASE + 0x0c)

#define RPPX1_IRQ_ID_256HIST			BIT(27)
#define RPPX1_IRQ_ID_PRE2_DPCC			BIT(25)
#define RPPX1_IRQ_ID_PRE1_DPCC			BIT(24)
#define RPPX1_IRQ_ID_MV_OUT_FRAME_OUT		BIT(23)
#define RPPX1_IRQ_ID_MV_OUT_OFF			BIT(22)
#define RPPX1_IRQ_ID_POST_AWB_MEAS		BIT(21)
#define RPPX1_IRQ_ID_POST_HIST_MEAS		BIT(20)
#define RPPX1_IRQ_ID_POST_TM			BIT(19)
#define RPPX1_IRQ_ID_PRE1_EXM			BIT(18)
#define RPPX1_IRQ_ID_PRE1_HIST			BIT(17)
#define RPPX1_IRQ_ID_PRE1_FRAME_IN		BIT(16)
#define RPPX1_IRQ_ID_PRE1_HSTART		BIT(15)
#define RPPX1_IRQ_ID_PRE1_VSTART		BIT(14)
#define RPPX1_IRQ_ID_PRE2_EXM			BIT(13)
#define RPPX1_IRQ_ID_PRE2_HIST			BIT(12)
#define RPPX1_IRQ_ID_PRE2_FRAME_IN		BIT(11)
#define RPPX1_IRQ_ID_PRE2_HSTART		BIT(10)
#define RPPX1_IRQ_ID_PRE2_VSTART		BIT(9)
#define RPPX1_IRQ_ID_OUT_FRAME			BIT(3)
#define RPPX1_IRQ_ID_OUT_OFF			BIT(2)
#define RPPX1_IRQ_ID_RMAP_MEAS			BIT(1)
#define RPPX1_IRQ_ID_RMAP_DONE			BIT(0)

struct rppx1 {
	struct device *dev;
	void __iomem *base;

	struct {
		struct rpp_module acq;
		struct rpp_module bls;
		struct rpp_module lin;
		struct rpp_module lsc;
		struct rpp_module awbg;
		struct rpp_module dpcc;
		struct rpp_module bd;
		struct rpp_module hist;
		struct rpp_module hist256;
		struct rpp_module exm;
	} pre1;

	struct {
		struct rpp_module acq;
		struct rpp_module bls;
		struct rpp_module lin;
		struct rpp_module lsc;
		struct rpp_module awbg;
		struct rpp_module dpcc;
		struct rpp_module bd;
		struct rpp_module hist;
		struct rpp_module exm;
	} pre2;

	struct {
		struct rpp_module awbg;
		struct rpp_module ccor;
		struct rpp_module hist;
		struct rpp_module db;
		struct rpp_module cac;
		struct rpp_module ltm;
		struct rpp_module ltmmeas;
		struct rpp_module wbmeas;
		struct rpp_module bdrgb;
		struct rpp_module shrp;
	} post;

	struct {
		struct rpp_module ga;
		struct rpp_module is;
		struct rpp_module ccor;
		struct rpp_module outif;
		struct rpp_module outregs;
	} hv;

	struct {
		struct rpp_module ga;
		struct rpp_module is;
		struct rpp_module ccor;
		struct rpp_module outif;
		struct rpp_module outregs;
		struct rpp_module xyz2luv;
	} mv;

	struct rpp_module rmap;
	struct rpp_module rmapmeas;
};

void rppx1_write(struct rppx1 *rpp, u32 offset, u32 value);
u32 rppx1_read(struct rppx1 *rpp, u32 offset);

#endif /* __MEDIA_RPPX1_H__ */
