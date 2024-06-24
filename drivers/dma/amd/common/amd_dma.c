// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AMD DMA Driver common
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Basavaraj Natikar <Basavaraj.Natikar@amd.com>
 */

#include "../common/amd_dma.h"

void pt_start_queue(struct pt_cmd_queue *cmd_q)
{
	/* Turn on the run bit */
	iowrite32(cmd_q->qcontrol | CMD_Q_RUN, cmd_q->reg_control);
}

void pt_stop_queue(struct pt_cmd_queue *cmd_q)
{
	/* Turn off the run bit */
	iowrite32(cmd_q->qcontrol & ~CMD_Q_RUN, cmd_q->reg_control);
}
