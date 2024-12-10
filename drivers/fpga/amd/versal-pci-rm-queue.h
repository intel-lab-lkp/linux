/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __RM_QUEUE_H
#define __RM_QUEUE_H

struct rm_device;

/* rm queue hardware setup */
int rm_queue_init(struct rm_device *rdev);
void rm_queue_fini(struct rm_device *rdev);

/* rm queue common API */
int rm_queue_send_cmd(struct rm_cmd *cmd, unsigned long timeout);
void rm_queue_withdraw_cmd(struct rm_cmd *cmd);

#endif	/* __RM_QUEUE_H */
