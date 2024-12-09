/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Device Tree constants for the Texas Instruments DP83822 PHY
 *
 * Copyright (C) 2024 Liebherr-Electronics and Drives GmbH
 *
 * Author: Dimitri Fedrau <dimitri.fedrau@liebherr.com>
 */

#ifndef _DT_BINDINGS_TI_DP83822_H
#define _DT_BINDINGS_TI_DP83822_H

/* IO_MUX_GPIO_CTRL - Clock source selection */
#define DP83822_CLK_SRC_MAC_IF			0x0
#define DP83822_CLK_SRC_XI			0x1
#define DP83822_CLK_SRC_INT_REF			0x2
#define DP83822_CLK_SRC_RMII_MASTER_MODE_REF	0x4
#define DP83822_CLK_SRC_FREE_RUNNING		0x6
#define DP83822_CLK_SRC_RECOVERED		0x7

#endif
