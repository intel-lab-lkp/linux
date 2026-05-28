/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_ARBITRATION_H__
#define __PANTHOR_ARBITRATION_H__

struct device;
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
};

#endif
