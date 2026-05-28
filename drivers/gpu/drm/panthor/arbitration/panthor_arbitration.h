/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_ARBITRATION_H__
#define __PANTHOR_ARBITRATION_H__

#include <linux/types.h>

struct device;
struct panthor_arbitration_sched;
struct panthor_partition_control;
struct panthor_resource_group;

#define AM_ARB_MAX_PC_COUNT			1
#define AM_ARB_MAX_RG_COUNT			1
#define AM_ARB_MAX_AW_COUNT			16

/**
 * struct panthor_arbitration - Arbitration device
 */
struct panthor_arbitration {
	/** @dev: Device pointer */
	struct device *dev;

	/** @pc: Pointer array to partition control data */
	struct panthor_partition_control *pc[AM_ARB_MAX_PC_COUNT];

	/** @rg: Pointer array to resource group data */
	struct panthor_resource_group *rg[AM_ARB_MAX_RG_COUNT];

	/** @shed: Pointer to scheduler data. 1 sched per partition */
	struct panthor_arbitration_sched *sched[AM_ARB_MAX_PC_COUNT];
};

/* AW to Arbiter events */
int panthor_arbitration_on_request(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_idle(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_stopped(struct panthor_arbitration *adev, u8 aw_id);

/* Arbiter to AW events */
int panthor_arbitration_on_grant(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_stop(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_close(struct panthor_arbitration *adev, u8 aw_id);

#endif
