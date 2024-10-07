/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VMGMT_RM_QUEUE_H
#define __VMGMT_RM_QUEUE_H

int rm_queue_init(struct rm_device *rdev);
void rm_queue_fini(struct rm_device *rdev);
int rm_queue_send_cmd(struct rm_cmd *cmd, unsigned long timeout);

#endif	/* __VMGMT_RM_QUEUE_H */
