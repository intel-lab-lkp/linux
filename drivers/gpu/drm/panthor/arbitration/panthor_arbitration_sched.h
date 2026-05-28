/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_ARBITRATION_SCHED_H__
#define __PANTHOR_ARBITRATION_SCHED_H__

#include <linux/types.h>

struct panthor_arbitration;
struct panthor_arbitration_sched;

int panthor_arbitration_sched_init(struct panthor_arbitration *adev);
void panthor_arbitration_sched_term(struct panthor_arbitration *adev);

int panthor_arbitration_sched_suspend(struct panthor_arbitration *adev);
int panthor_arbitration_sched_resume(struct panthor_arbitration *adev);

int panthor_arbitration_sched_stop(struct panthor_arbitration_sched *sched);
int panthor_arbitration_sched_start(struct panthor_arbitration_sched *sched);

int panthor_arbitration_sched_on_request(struct panthor_arbitration_sched *sched, u8 aw_id);
int panthor_arbitration_sched_on_idle(struct panthor_arbitration_sched *sched, u8 aw_id);
int panthor_arbitration_sched_on_stopped(struct panthor_arbitration_sched *sched, u8 aw_id);

#endif
