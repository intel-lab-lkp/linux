/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_DEADLINE_MGR_H_
#define _XE_DEADLINE_MGR_H_

#include <linux/types.h>

struct dma_fence;
struct xe_deadline_mgr;
struct xe_exec_queue;

void xe_deadline_mgr_init(struct xe_deadline_mgr *mgr, struct xe_exec_queue *q);

void xe_deadline_mgr_fini(struct xe_deadline_mgr *mgr);

void xe_deadline_mgr_add_deadline(struct xe_deadline_mgr *mgr,
				  struct dma_fence *fence,
				  ktime_t deadline);

void xe_deadline_mgr_remove_deadline(struct xe_deadline_mgr *mgr,
				     struct dma_fence *fence);

#endif
