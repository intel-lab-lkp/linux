/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * On-board switches for the Renesas RZ/N1D demo board (DB)
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#ifndef __R9A06G032_RZN1D400_DB_SWITCHES_H
#define __R9A06G032_RZN1D400_DB_SWITCHES_H

#define SW_OFF         0
#define SW_ON          1

/*
 * SW2-2:
 *     SW_OFF		- enable ALT0_PMOD (PMOD1-3 + LEDs on the extension board)
 *     SW_ON (default)	- enable ALT1_CAT_S3 (CAT/S3)
 */
#define DB_SW2_2 SW_ON

#endif
