/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2019 Collabora ltd. */

#ifndef __PANTHOR_COREDUMP_H__
#define __PANTHOR_COREDUMP_H__

#include <drm/panthor_drm.h>
#include <linux/sched.h>
#include <linux/types.h>

struct panthor_coredump;
struct panthor_device;
struct panthor_group;

/**
 * enum panthor_coredump_reason - Coredump reason
 */
enum panthor_coredump_reason {
	PANTHOR_COREDUMP_REASON_MMU_FAULT,
	PANTHOR_COREDUMP_REASON_CSG_REQ_TIMEOUT,
	PANTHOR_COREDUMP_REASON_CSG_UNKNOWN_STATE,
	PANTHOR_COREDUMP_REASON_CSG_PROGRESS_TIMEOUT,
	PANTHOR_COREDUMP_REASON_CS_FATAL,
	PANTHOR_COREDUMP_REASON_CS_FAULT,
	PANTHOR_COREDUMP_REASON_CS_TILER_OOM,
	PANTHOR_COREDUMP_REASON_JOB_TIMEOUT,
};

/**
 * struct panthor_coredump_group_state - Coredump group state
 *
 * Interesting panthor_group fields.
 */
struct panthor_coredump_group_state {
	enum drm_panthor_group_priority priority;
	u32 queue_count;
	pid_t pid;
	char comm[TASK_COMM_LEN];
	bool destroyed;
	int csg_id;
};

#ifdef CONFIG_DEV_COREDUMP

struct panthor_coredump *
panthor_coredump_alloc(struct panthor_device *ptdev,
		       enum panthor_coredump_reason reason, gfp_t gfp);

void panthor_coredump_capture(struct panthor_coredump *cd,
			      struct panthor_group *group);

#else /* CONFIG_DEV_COREDUMP */

static inline struct panthor_coredump *
panthor_coredump_alloc(struct panthor_device *ptdev,
		       enum panthor_coredump_reason reason, gfp_t gfp)
{
	return NULL;
}

static inline void panthor_coredump_capture(struct panthor_coredump *cd,
					    struct panthor_group *group)
{
}

#endif /* CONFIG_DEV_COREDUMP */

#endif /* __PANTHOR_COREDUMP_H__ */
