/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#ifndef __CLOCK_TOSHIBA_TC9564_H__
#define __CLOCK_TOSHIBA_TC9564_H__

/* Clock IDs */

#define CLOCK_I2C		0	/* critical */
#define CLOCK_PCIE		1	/* critical */
#define CLOCK_MSIGEN		2

#define CLOCK_MAC0_TX		3
#define CLOCK_MAC0_RX		4
#define CLOCK_MAC0_125M		5
#define CLOCK_MAC0_ALL		6

#define CLOCK_MAC1_TX		7
#define CLOCK_MAC1_RX		8
#define CLOCK_MAC1_125M		9
#define CLOCK_MAC1_ALL		10

/* Reset IDs */

#define RESET_I2C		0	/* critical */
#define RESET_PCIE		1	/* critical */
#define RESET_MSIGEN		2

#define RESET_MAC0_MAC		3
#define RESET_MAC0_PMA		4
#define RESET_MAC0_XPCS		5

#define RESET_MAC1_MAC		6
#define RESET_MAC1_PMA		7
#define RESET_MAC1_XPCS		8

#endif /* __CLOCK_TOSHIBA_TC9564_H__*/
