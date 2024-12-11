/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2024 Cirrus Logic, Inc. and
 *		      Cirrus Logic International Simiconductor Ltd.
 *
 * Author: Paul Handrigan <paulha@opensource.cirrus.com>
 *
 */

#ifndef _DT_BINDINGS_CLK_CIRRUS_CS2600_H
#define _DT_BINDINGS_CLK_CIRRUS_CS2600_H

/* CS2600 Clock Outputs  */
#define CS2600_CLK_OUTPUT		0
#define CS2600_BCLK_OUTPUT		1
#define CS2600_FSYNC_OUTPUT		2

/* CS2600 Auxiliary Output */
#define CS2600_AUX_OUTPUT_FREQ_UNLOCK	0
#define CS2600_AUX_OUTPUT_PHASE_UNLOCK	1
#define CS2600_AUX_OUTPUT_NO_CLKIN	2

#endif
