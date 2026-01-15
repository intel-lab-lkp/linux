/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Tenstorrent
 */
#ifndef __SOC_ATLANTIS_SYSCON_H__
#define __SOC_ATLANTIS_SYSCON_H__

#include <linux/bits.h>
#include <linux/types.h>

/* RCPU Clock Register Offsets */
#define RCPU_PLL_CFG_REG 0x0000
#define RCPU_NOCC_PLL_CFG_REG 0x0004
#define RCPU_NOCC_CLK_CFG_REG 0x0008
#define RCPU_DIV_CFG_REG 0x000C
#define RCPU_BLK_CG_REG 0x0014
#define LSIO_BLK_CG_REG 0x0018
#define PLL_RCPU_EN_REG 0x11c
#define PLL_NOCC_EN_REG 0x120
#define BUS_CG_REG 0x01FC

/* PLL Bit Definitions */
#define PLL_CFG_EN_BIT BIT(0)
#define PLL_CFG_BYPASS_BIT BIT(1)
#define PLL_CFG_REFDIV_MASK GENMASK(7, 2)
#define PLL_CFG_REFDIV_SHIFT 2
#define PLL_CFG_POSTDIV1_MASK GENMASK(10, 8)
#define PLL_CFG_POSTDIV1_SHIFT 8
#define PLL_CFG_POSTDIV2_MASK GENMASK(13, 11)
#define PLL_CFG_POSTDIV2_SHIFT 11
#define PLL_CFG_FBDIV_MASK GENMASK(25, 14)
#define PLL_CFG_FBDIV_SHIFT 14
#define PLL_CFG_LKDT_BIT BIT(30)
#define PLL_CFG_LOCK_BIT BIT(31)
#define PLL_LOCK_TIMEOUT_US 1000
#define PLL_BYPASS_WAIT_US 500

#endif
