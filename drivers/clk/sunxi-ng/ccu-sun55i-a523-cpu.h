/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025 Arm Ltd.
 */

#ifndef _CCU_SUN55I_A523_CPU_H
#define _CCU_SUN55I_A523_CPU_H

#include <dt-bindings/clock/sun55i-a523-cpu-ccu.h>

/* The PLL clocks itself and the pure divider clocks are not exported. */

#define CLK_PLL_CPU_0		0
#define CLK_PLL_CPU_L		1
#define CLK_PLL_CPU_DSU		2
#define CLK_PLL_CPU_B		3

#define CLK_DIV_CPU_L		4
#define CLK_DIV_CPU_DSU		5
#define CLK_DIV_CPU_B		6

#define CLK_NUMBER	(CLK_CPU_B + 1)

#endif /* _CCU_SUN55I_A523_CPU_H */
