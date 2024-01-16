/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_LOG_TYPES_H_
#define _XE_GUC_LOG_TYPES_H_

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include "xe_guc_fwif.h"

enum {
	GUC_LOG_SECTIONS_CRASH,
	GUC_LOG_SECTIONS_DEBUG,
	GUC_LOG_SECTIONS_CAPTURE,
	GUC_LOG_SECTIONS_LIMIT
};

struct xe_bo;

/**
 * struct xe_guc_log - GuC log
 */
struct xe_guc_log {
	/** @level: GuC log level */
	u32 level;
	/** @bo: XE BO for GuC log */
	struct xe_bo *bo;

	/* Allocation settings */
	struct {
		s32 bytes;	/* Size in bytes */
		s32 units;	/* GuC API units - 1MB or 4KB */
		s32 count;	/* Number of API units */
		u32 flag;	/* GuC API units flag */
	} sizes[GUC_LOG_SECTIONS_LIMIT];
	bool sizes_initialised;

	/* logging related stats */
	struct {
		u32 sampled_overflow;
		u32 overflow;
		u32 flush;
	} stats[GUC_MAX_LOG_BUFFER];
};

#endif
