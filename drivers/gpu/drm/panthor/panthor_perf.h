/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 Collabora Ltd */
/* Copyright 2025 Arm ltd. */

#ifndef __PANTHOR_PERF_H__
#define __PANTHOR_PERF_H__

#include <linux/types.h>

struct panthor_device;

int panthor_perf_init(struct panthor_device *ptdev);
void panthor_perf_unplug(struct panthor_device *ptdev);

#endif /* __PANTHOR_PERF_H__ */

