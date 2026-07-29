/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */

#ifndef RVU_SWITCH_H
#define RVU_SWITCH_H

/* RVU Switch */
#define RVU_SW_INVALID_PORT_ID	((u32)~0U)

u32 rvu_sw_port_id(struct rvu *rvu, u16 pcifunc);

#endif
