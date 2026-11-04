/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#ifndef __SOC_TOSHIBA_TC9564_H__
#define __SOC_TOSHIBA_TC9564_H__

/* Clock IDs */

#define CLOCK_PLL		0

#define CLOCK_62_5M		1
#define CLOCK_125M		2
#define CLOCK_250M		3
#define CLOCK_312_5M		4

#define CLOCK_MSIGEN		5

#define CLOCK_MAC0_TX		6
#define CLOCK_MAC0_RX		7
#define CLOCK_MAC0_125M		8
#define CLOCK_MAC0_312_5M	9
#define CLOCK_MAC0_ALL		10

#define CLOCK_MAC1_TX		11
#define CLOCK_MAC1_RX		12
#define CLOCK_MAC1_125M		13
#define CLOCK_MAC1_312_5M	14
#define CLOCK_MAC1_ALL		15
#define CLOCK_MAC1_RMII		16

/* Reset IDs */

#define RESET_MSIGEN		0

#define RESET_MAC0_MAC		1
#define RESET_MAC0_PMA		2
#define RESET_MAC0_XPCS		3

#define RESET_MAC1_MAC		4
#define RESET_MAC1_PMA		5
#define RESET_MAC1_XPCS		6

#endif /* __SOC_TOSHIBA_TC9564_H__*/
