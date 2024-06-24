/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AMD DMA Driver common
 *
 * Copyright (c) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Basavaraj Natikar <Basavaraj.Natikar@amd.com>
 */

#ifndef AMD_DMA_H
#define AMD_DMA_H

#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "../ptdma/ptdma.h"
#include "../../virt-dma.h"

void pt_start_queue(struct pt_cmd_queue *cmd_q);
void pt_stop_queue(struct pt_cmd_queue *cmd_q);

#endif
