/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_ARBITRATION_SCHED_H__
#define __PANTHOR_ARBITRATION_SCHED_H__

#include <linux/types.h>

#include "panthor_arbitration.h"

struct panthor_arbitration;
struct panthor_arbitration_sched;

enum panthor_arb_sched_disable_reason {
	/** @ARB_SCHED_DISABLE_PM: scheduler disabled due to PM suspend */
	ARB_SCHED_DISABLE_PM = 0,

	/** @ARB_SCHED_DISABLE_TERM: scheduler disabled for termination */
	ARB_SCHED_DISABLE_TERM,

	/** @ARB_SCHED_DISABLE_RESOURCE: No resource assigned. scheduler disabled. */
	ARB_SCHED_DISABLE_RESOURCE,
};

int panthor_arbitration_sched_init(struct panthor_arbitration *adev);
void panthor_arbitration_sched_term(struct panthor_arbitration *adev);
void panthor_arbitration_sched_cancel(struct panthor_arbitration *adev);

int panthor_arbitration_sched_suspend(struct panthor_arbitration *adev);
int panthor_arbitration_sched_resume(struct panthor_arbitration *adev);

int panthor_arbitration_sched_stop(struct panthor_arbitration_sched *sched);
int panthor_arbitration_sched_start(struct panthor_arbitration_sched *sched);

void panthor_arbitration_sched_disable_set(struct panthor_arbitration_sched *sched,
					   enum panthor_arb_sched_disable_reason reason);
void panthor_arbitration_sched_disable_clear(struct panthor_arbitration_sched *sched,
					     enum panthor_arb_sched_disable_reason reason);

void panthor_arbitration_sched_reset(struct panthor_arbitration_sched *sched, u8 aw_id);

int panthor_arbitration_sched_on_request(struct panthor_arbitration_sched *sched, u8 aw_id);
int panthor_arbitration_sched_on_idle(struct panthor_arbitration_sched *sched, u8 aw_id);
int panthor_arbitration_sched_on_stopped(struct panthor_arbitration_sched *sched, u8 aw_id);

#endif
