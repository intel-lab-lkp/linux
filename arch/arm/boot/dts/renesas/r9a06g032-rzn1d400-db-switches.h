/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * On-board switches for the Renesas RZ/N1D demo board (DB) and extension
 * board (EB)
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#ifndef __N1D_DB_EB_SWITCHES_H__
#define __N1D_DB_EB_SWITCHES_H__

#define SW_OFF         0
#define SW_ON          1

/*
 * SW_2-2:
 *     SW_OFF - enable PMOD1-3+LEDs on the extension board
 *     SW_ON  - enable CAT/S3 (default)
 */
#define SW_2_2 SW_ON

#endif
