/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2025 Intel Corporation
 */

#ifndef __INTEL_CMTG_REGS_H__
#define __INTEL_CMTG_REGS_H__

#include "intel_display_reg_defs.h"

#define CMTG_CLK_SEL			_MMIO(0x46160)
#define CMTG_CLK_SEL_A_MASK		REG_GENMASK(31, 29)
#define CMTG_CLK_SELECT_PHYA_ENABLE	REG_FIELD_PREP(CMTG_CLK_SEL_A_MASK, 0x4)
#define CMTG_CLK_SEL_A_DISABLED		REG_FIELD_PREP(CMTG_CLK_SEL_A_MASK, 0)
#define CMTG_CLK_SEL_B_MASK		REG_GENMASK(15, 13)
#define CMTG_CLK_SELECT_PHYB_ENABLE	REG_FIELD_PREP(CMTG_CLK_SEL_A_MASK, 0x6)
#define CMTG_CLK_SEL_B_DISABLED		REG_FIELD_PREP(CMTG_CLK_SEL_B_MASK, 0)

#define TRANS_CMTG_CTL_A		_MMIO(0x6fa88)
#define TRANS_CMTG_CTL_B		_MMIO(0x6fb88)
#define  CMTG_ENABLE			REG_BIT(31)

#define TRANS_HTOTAL_CMTG(trans)	_MMIO(0x6F000 + (trans) * 0x100)
#define TRANS_HBLANK_CMTG(trans)	_MMIO(0x6F004 + (trans) * 0x100)
#define TRANS_HSYNC_CMTG(trans)		_MMIO(0x6F008 + (trans) * 0x100)
#define TRANS_VTOTAL_CMTG(trans)	_MMIO(0x6F00C + (trans) * 0x100)
#define TRANS_VBLANK_CMTG(trans)	_MMIO(0x6F010 + (trans) * 0x100)
#define TRANS_VSYNC_CMTG(trans)		_MMIO(0x6F014 + (trans) * 0x100)

#define TRANS_SET_CTX_LATENCY_CMTG(trans)	_MMIO(0x6F07C + (trans) * 0x100)

#define TRANS_VRR_CTL_CMTG(trans)	_MMIO(0x6F420 + (trans) * 0x100)
#define TRANS_VRR_VMAX_CMTG(trans)	_MMIO(0x6F424 + (trans) * 0x100)
#define TRANS_VRR_VMIN_CMTG(trans)	_MMIO(0x6F434 + (trans) * 0x100)
#define TRANS_VRR_FLIPLINE_CMTG(trans)	_MMIO(0x6F438 + (trans) * 0x100)

#endif /* __INTEL_CMTG_REGS_H__ */
