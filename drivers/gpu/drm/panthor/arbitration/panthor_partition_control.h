/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_PARTITION_CONTROL_H__
#define __PANTHOR_PARTITION_CONTROL_H__

#include <linux/types.h>

struct device;
struct panthor_arbitration;
struct panthor_partition_control;

int panthor_partition_control_init(struct panthor_arbitration *adev);

void panthor_partition_control_term(struct panthor_arbitration *adev);

int panthor_partition_control_suspend(struct panthor_arbitration *adev);

int panthor_partition_control_resume(struct panthor_arbitration *adev);

int panthor_partition_control_open_window(struct panthor_partition_control *pc, u8 aw_id);

int panthor_partition_control_close_window(struct panthor_partition_control *pc);

int panthor_partition_control_yield_now(struct panthor_partition_control *pc);

#endif
