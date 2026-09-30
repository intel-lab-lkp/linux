/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * This header provides constants for TI's TDA54 SoC device tree sources.
 *
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */
#ifndef DTS_ARM64_TI_K3_TDA54_PINCTRL_H
#define DTS_ARM64_TI_K3_TDA54_PINCTRL_H

#define TDA54_PINMUX(pad, mux)		(((pad) << 8) | ((mux) & 0xf))

#define TDA54_MUX_MODE0		0
#define TDA54_MUX_MODE1		1
#define TDA54_MUX_MODE2		2
#define TDA54_MUX_MODE3		3
#define TDA54_MUX_MODE4		4
#define TDA54_MUX_MODE5		5
#define TDA54_MUX_MODE6		6
#define TDA54_MUX_MODE7		7
#define TDA54_MUX_MODE8		8
#define TDA54_MUX_MODE9		9
#define TDA54_MUX_MODE10		10
#define TDA54_MUX_MODE11		11
#define TDA54_MUX_MODE12		12
#define TDA54_MUX_MODE13		13
#define TDA54_MUX_MODE14		14
#define TDA54_MUX_MODE15		15

#endif
