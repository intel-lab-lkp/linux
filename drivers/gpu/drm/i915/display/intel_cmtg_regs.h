/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Intel Corporation
 */

#ifndef __INTEL_CMTG_REGS_H__
#define __INTEL_CMTG_REGS_H__

#include "intel_display_reg_defs.h"

enum cmtg {
	CMTG_A = 0,
	CMTG_B,
	MAX_CMTG
};

#define CMTG_CLK_SEL			_MMIO(0x46160)
#define CMTG_CLK_SEL_A_MASK		REG_GENMASK(31, 29)
#define CMTG_CLK_SELECT_PHYA_ENABLE	0x4
#define CMTG_CLK_SEL_A_DISABLED		REG_FIELD_PREP(CMTG_CLK_SEL_A_MASK, 0)
#define CMTG_CLK_SEL_B_MASK		REG_GENMASK(15, 13)
#define CMTG_CLK_SELECT_PHYB_ENABLE	0x6
#define CMTG_CLK_SEL_B_DISABLED		REG_FIELD_PREP(CMTG_CLK_SEL_B_MASK, 0)

#define TRANS_CMTG_CTL_A		_MMIO(0x6fa88)
#define TRANS_CMTG_CTL_B		_MMIO(0x6fb88)
#define  CMTG_ENABLE			REG_BIT(31)

#define TRANS_HTOTAL_CMTG(id)		_MMIO(0x6F000 + (id) * 0x100)
#define TRANS_HBLANK_CMTG(id)		_MMIO(0x6F004 + (id) * 0x100)
#define TRANS_HSYNC_CMTG(id)		_MMIO(0x6F008 + (id) * 0x100)
#define TRANS_VTOTAL_CMTG(id)		_MMIO(0x6F00C + (id) * 0x100)
#define TRANS_VBLANK_CMTG(id)		_MMIO(0x6F010 + (id) * 0x100)
#define TRANS_VSYNC_CMTG(id)		_MMIO(0x6F014 + (id) * 0x100)

#define TRANS_SET_CTX_LATENCY_CMTG(id)	_MMIO(0x6F07C + (id) * 0x100)

#define TRANS_VRR_CTL_CMTG(id)		_MMIO(0x6F420 + (id) * 0x100)
#define TRANS_VRR_VMAX_CMTG(id)		_MMIO(0x6F424 + (id) * 0x100)
#define TRANS_VRR_VMIN_CMTG(id)		_MMIO(0x6F434 + (id) * 0x100)
#define TRANS_VRR_FLIPLINE_CMTG(id)	_MMIO(0x6F438 + (id) * 0x100)

#endif /* __INTEL_CMTG_REGS_H__ */
