/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_PREEMPT_FENCE_TYPES_H_
#define _XE_PREEMPT_FENCE_TYPES_H_

#include <linux/dma-fence-preempt.h>

struct xe_exec_queue;

/**
 * struct xe_preempt_fence - XE preempt fence
 *
 * hardware and triggers a callback once the xe_engine is complete.
 */
struct xe_preempt_fence {
	/** @base: dma fence base */
	struct dma_fence_preempt base;
	/** @link: link into list of pending preempt fences */
	struct list_head link;
	/** @q: exec queue for this preempt fence */
	struct xe_exec_queue *q;
};

#endif
