/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_DSS_REGS_H__
#define __INTEL_DSS_REGS_H__

#include "intel_display_reg_defs.h"

/* Display Stream Splitter Control */
#define DSS_CTL1				_MMIO(0x67400)
#define  SPLITTER_ENABLE			(1 << 31)
#define  JOINER_ENABLE				(1 << 30)
#define  DUAL_LINK_MODE_INTERLEAVE		(1 << 24)
#define  DUAL_LINK_MODE_FRONTBACK		(0 << 24)
#define  OVERLAP_PIXELS_MASK			(0xf << 16)
#define  OVERLAP_PIXELS(pixels)			((pixels) << 16)
#define  LEFT_DL_BUF_TARGET_DEPTH_MASK		(0xfff << 0)
#define  LEFT_DL_BUF_TARGET_DEPTH(pixels)	((pixels) << 0)
#define  MAX_DL_BUFFER_TARGET_DEPTH		0x5a0

#define DSS_CTL2				_MMIO(0x67404)
#define  LEFT_BRANCH_VDSC_ENABLE		(1 << 31)
#define  RIGHT_BRANCH_VDSC_ENABLE		(1 << 15)
#define  RIGHT_DL_BUF_TARGET_DEPTH_MASK		(0xfff << 0)
#define  RIGHT_DL_BUF_TARGET_DEPTH(pixels)	((pixels) << 0)

#define _ICL_PIPE_DSS_CTL1_PB			0x78200
#define _ICL_PIPE_DSS_CTL1_PC			0x78400
#define ICL_PIPE_DSS_CTL1(pipe)			_MMIO_PIPE((pipe) - PIPE_B, \
							   _ICL_PIPE_DSS_CTL1_PB, \
							   _ICL_PIPE_DSS_CTL1_PC)
#define  BIG_JOINER_ENABLE			(1 << 29)
#define  PRIMARY_BIG_JOINER_ENABLE		(1 << 28)
#define  VGA_CENTERING_ENABLE			(1 << 27)
#define  SPLITTER_CONFIGURATION_MASK		REG_GENMASK(26, 25)
#define  SPLITTER_CONFIGURATION_2_SEGMENT	REG_FIELD_PREP(SPLITTER_CONFIGURATION_MASK, 0)
#define  SPLITTER_CONFIGURATION_4_SEGMENT	REG_FIELD_PREP(SPLITTER_CONFIGURATION_MASK, 1)
#define  UNCOMPRESSED_JOINER_PRIMARY		(1 << 21)
#define  UNCOMPRESSED_JOINER_SECONDARY		(1 << 20)

#define _ICL_PIPE_DSS_CTL2_PB			0x78204
#define _ICL_PIPE_DSS_CTL2_PC			0x78404
#define ICL_PIPE_DSS_CTL2(pipe)			_MMIO_PIPE((pipe) - PIPE_B, \
							   _ICL_PIPE_DSS_CTL2_PB, \
							   _ICL_PIPE_DSS_CTL2_PC)

#endif /* __INTEL_DSS_REGS_H__ */
