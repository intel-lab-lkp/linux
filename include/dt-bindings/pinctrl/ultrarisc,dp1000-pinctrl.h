/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * UltraRISC DP1000 pinctrl header.
 *
 * Copyright (C) 2026 UltraRISC Technology (Shanghai) Co., Ltd.
 */

#ifndef _DT_BINDINGS_PINCTRL_ULTRARISC_DP1000_PINCTRL_H
#define _DT_BINDINGS_PINCTRL_ULTRARISC_DP1000_PINCTRL_H

/**
 * UltraRISC DP1000 IO pad configuration
 * port: A, B, C, D, LPC
 *     Pin in the port
 * pin:
 *     PA: 0 - 15
 *     PB-PD: 0 - 7
 *     LPC: 0 - 12
 * func:
 *     UR_DP1000_FUNC_DEF: default
 *     UR_DP1000_FUNC0: func0
 *     UR_DP1000_FUNC1: func1
 */
#define UR_DP1000_IOMUX_A		0x0
#define UR_DP1000_IOMUX_B		0x1
#define UR_DP1000_IOMUX_C		0x2
#define UR_DP1000_IOMUX_D		0x3
#define UR_DP1000_IOMUX_LPC		0x4

#define UR_DP1000_FUNC_DEF		0
#define UR_DP1000_FUNC0			1
#define UR_DP1000_FUNC1			0x10000

/**
 * Configure pull up/down resistor of the IO pin
 * UR_DP1000_PULL_DIS: disable pull-up and pull-down
 * UR_DP1000_PULL_UP: enable pull-up
 * UR_DP1000_PULL_DOWN: enable pull-down
 */
#define UR_DP1000_PULL_DIS	0
#define UR_DP1000_PULL_UP	1
#define UR_DP1000_PULL_DOWN	2
/**
 * Configure drive strength of the IO pin
 * UR_DP1000_DRIVE_DEF: default value, reset value is 2
 * UR_DP1000_DRIVE_0: 20mA
 * UR_DP1000_DRIVE_1: 27mA
 * UR_DP1000_DRIVE_2: 33mA
 * UR_DP1000_DRIVE_3: 40mA
 */
#define UR_DP1000_DRIVE_DEF	2
#define UR_DP1000_DRIVE_0	0
#define UR_DP1000_DRIVE_1	1
#define UR_DP1000_DRIVE_2	2
#define UR_DP1000_DRIVE_3	3

/**
 * Combine the pull-up/down resistor and drive strength
 * pull: UR_DP1000_PULL_DIS, UR_DP1000_PULL_UP, UR_DP1000_PULL_DOWN
 * drive: UR_DP1000_DRIVE_DEF, UR_DP1000_DRIVE_0, UR_DP1000_DRIVE_1,
 *        UR_DP1000_DRIVE_2, UR_DP1000_DRIVE_3
 */
#define UR_DP1000_BIAS(pull, drive)		(((pull) << 2) | (drive))

#endif /* _DT_BINDINGS_PINCTRL_ULTRARISC_DP1000_PINCTRL_H */
