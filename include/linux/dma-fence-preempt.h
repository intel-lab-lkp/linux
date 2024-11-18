/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __LINUX_DMA_FENCE_PREEMPT_H
#define __LINUX_DMA_FENCE_PREEMPT_H

#include <linux/dma-fence.h>
#include <linux/workqueue.h>

struct dma_fence_preempt;
struct dma_resv;

/**
 * struct dma_fence_preempt_ops - Preempt fence operations
 *
 * These functions should be implemented in the driver side.
 */
struct dma_fence_preempt_ops {
	/** @preempt_delay: Preempt execution with a delay */
	struct dma_fence *(*preempt_delay)(struct dma_fence_preempt *fence);
	/** @preempt: Preempt execution */
	int (*preempt)(struct dma_fence_preempt *fence);
	/** @preempt_wait: Wait for preempt of execution to complete */
	int (*preempt_wait)(struct dma_fence_preempt *fence);
	/** @preempt_finished: Signal that the preempt has finished */
	void (*preempt_finished)(struct dma_fence_preempt *fence);
};

/**
 * struct dma_fence_preempt - Embedded preempt fence base class
 */
struct dma_fence_preempt {
	/** @base: Fence base class */
	struct dma_fence base;
	/** @lock: Spinlock for fence handling */
	spinlock_t lock;
	/** @cb: Callback preempt delay */
	struct dma_fence_cb cb;
	/** @ops: Preempt fence operation */
	const struct dma_fence_preempt_ops *ops;
	/** @wq: Work queue for preempt wait */
	struct workqueue_struct *wq;
	/** @work: Work struct for preempt wait */
	struct work_struct work;
};

bool dma_fence_is_preempt(const struct dma_fence *fence);

void dma_fence_preempt_init(struct dma_fence_preempt *fence,
			    const struct dma_fence_preempt_ops *ops,
			    struct workqueue_struct *wq,
			    u64 context, u64 seqno);

#endif
