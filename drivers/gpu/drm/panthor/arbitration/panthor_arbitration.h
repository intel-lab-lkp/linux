/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_ARBITRATION_H__
#define __PANTHOR_ARBITRATION_H__

#include <linux/ioport.h>
#include <linux/fwnode.h>
#include <linux/types.h>

struct device;
struct panthor_arbitration_sched;
struct panthor_partition_control;
struct panthor_resource_group;

#define AM_ARB_MAX_PC_COUNT			1
#define AM_ARB_MAX_RG_COUNT			1
#define AM_ARB_MAX_AW_COUNT			16

/** struct panthor_arbitration_resource - Arbitration resource */
struct panthor_arbitration_resource {
	/** @res: resource parsed from child 'reg' property */
	struct resource res;

	/** @id: id parsed from child 'arm,id' property */
	u32 id;
};

/**
 * struct panthor_arbitration - Arbitration device
 */
struct panthor_arbitration {
	/** @dev: Device pointer */
	struct device *dev;

	/** @tearing_down: Driver remove in progress */
	bool tearing_down;

	/** @pc: Pointer array to partition control data */
	struct panthor_partition_control *pc[AM_ARB_MAX_PC_COUNT];

	/** @rg: Pointer array to resource group data */
	struct panthor_resource_group *rg[AM_ARB_MAX_RG_COUNT];

	/** @sched: Pointer to scheduler data. 1 sched per partition */
	struct panthor_arbitration_sched *sched[AM_ARB_MAX_PC_COUNT];
};

int panthor_arbitration_parse_resource(struct device *dev,
				       const struct fwnode_handle *child,
				       struct panthor_arbitration_resource *res);

int panthor_arbitration_on_grant(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_stop(struct panthor_arbitration *adev, u8 aw_id);
int panthor_arbitration_on_close(struct panthor_arbitration *adev, u8 aw_id);

#endif
