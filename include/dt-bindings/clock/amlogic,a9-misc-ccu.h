/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (C) 2026 Amlogic, Inc. All rights reserved
 */

#ifndef __AMLOGIC_A9_MISC_CCU_H
#define __AMLOGIC_A9_MISC_CCU_H

/* &clkc_sc (Smart Card) */
#define A9_CLK_SC_PRE				0
#define A9_CLK_SC				1

/* &clkc_ts (Temperature Sensor) */
#define A9_CLK_TS_DIV				0
#define A9_CLK_TS				1

/* &clkc_gen_out (Generate Output) */
#define A9_CLK_GENOUT_SEL			0
#define A9_CLK_GENOUT_DIV			1
#define A9_CLK_GENOUT				2

/* &clkc_12_24m (12M & 24M) */
#define A9_CLK_24M_IN				0
#define A9_CLK_12_24M				1

/* &clkc_vapb_ge2d (VAPB & GE2D) */
#define A9_CLK_VAPB				0
#define A9_CLK_GE2D				1

/* &clkc_di (Deinterlacer) */
#define A9_CLK_VPU_CLKB_TEMP			0
#define A9_CLK_VPU_CLKB_DIV			1
#define A9_CLK_VPU_CLKB				2

/* &clkc_eth (ETH) */
#define A9_CLK_ETH_125M				0
#define A9_CLK_ETH_RMII				1

/* &clkc_mclk or &clkc_mclk1 (mclk-ccu) */
#define A9_CLK_MCLK_0_PRE_DIV			0
#define A9_CLK_MCLK_0_SEL			1
#define A9_CLK_MCLK_0_DIV			2
#define A9_CLK_MCLK_0				3
#define A9_CLK_MCLK_1_PRE_DIV			4
#define A9_CLK_MCLK_1_SEL			5
#define A9_CLK_MCLK_1_DIV			6
#define A9_CLK_MCLK_1				7

/* &clkc_ao_cecb, &clkc_ao_rtc, &clkc_rtc (dualdivmux-ccu) */
#define A9_CLK_DUALDIV				0
#define A9_CLK_DUALDIV_SEL			1

#endif /* __AMLOGIC_A9_MISC_CCU_H */
