// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#include "vmgmt.h"
#include "vmgmt-rm.h"
#include "vmgmt-rm-queue.h"

int rm_queue_send_cmd(struct rm_cmd *cmd, unsigned long timeout)
{
	return 0;
}

void rm_queue_fini(struct rm_device *rdev)
{
}

int rm_queue_init(struct rm_device *rdev)
{
	return 0;
}
