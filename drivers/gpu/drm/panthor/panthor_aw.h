/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_AW_H__
#define __PANTHOR_AW_H__

#include <linux/types.h>

struct panthor_device;

/**
 * enum aw_states - Enumeration of possible Access Window states
 */
enum aw_states {
	/** @PANTHOR_AW_STATE_INIT: Initial state, prior to handshake. */
	PANTHOR_AW_STATE_INIT = 0,

	/** @PANTHOR_AW_STATE_READY: Handshake with Resource Group completed. */
	PANTHOR_AW_STATE_READY,

	/** @PANTHOR_AW_STATE_GPU_REQUEST: AW requested for GPU access. */
	PANTHOR_AW_STATE_GPU_REQUEST,

	/** @PANTHOR_AW_STATE_GPU_GRANTED: AW is granted GPU access. */
	PANTHOR_AW_STATE_GPU_GRANTED,

	/** @PANTHOR_AW_STATE_STOPPED_IDLE: AW has stopped GPU access. */
	PANTHOR_AW_STATE_STOPPED_IDLE,

	/** @PANTHOR_AW_STATE_GPU_STOPPED: Window was closed, cleanup required. */
	PANTHOR_AW_STATE_GPU_STOPPED,
};

int panthor_aw_init(struct panthor_device *ptdev);

void panthor_aw_unplug(struct panthor_device *ptdev);

int panthor_aw_resume(struct panthor_device *ptdev);

int panthor_aw_suspend(struct panthor_device *ptdev);

int panthor_aw_ensure_gpu_access(struct panthor_device *ptdev);

#endif
