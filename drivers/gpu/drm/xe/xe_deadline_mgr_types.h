/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_DEADLINE_MGR_TYPES_H_
#define _XE_DEADLINE_MGR_TYPES_H_

#include <linux/hrtimer_types.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct xe_exec_queue;

#define XE_DEADLINE_NONE	(-1)
#define XE_DEADLINE_DONE	(-2)

/** enum xe_deadline_mgr_state - Deadline manager state */
enum xe_deadline_mgr_state {
	/** @XE_DEADLINE_MGR_STATE_UNSUPPORTED: Unsupported (disabled) */
	XE_DEADLINE_MGR_STATE_UNSUPPORTED,
	/** @XE_DEADLINE_MGR_STATE_NO_BOOST: No boosted state */
	XE_DEADLINE_MGR_STATE_NO_BOOST,
	/** @XE_DEADLINE_MGR_STATE_FREQ_BOOST: Frequency boosted state */
	XE_DEADLINE_MGR_STATE_FREQ_BOOST,
	/** @XE_DEADLINE_MGR_STATE_PRIO_BOOST: Priority boosted state */
	XE_DEADLINE_MGR_STATE_PRIO_BOOST,
};

/** struct xe_deadline_mgr - Xe deadline manager */
struct xe_deadline_mgr {
	/** @q: Pointer to queue associated with deadline */
	struct xe_exec_queue *q;
	/** @deadlines: List storing deadline fences, protected by @lock */
	struct list_head deadlines;
	/** @timer: Timer to enter deadline mode, protected by @lock */
	struct hrtimer timer;
	/**
	 * @exit_delay: Delayed worker to exit deadline mode, protected by
	 * @lock
	 */
	struct delayed_work exit_delay;
	/** @lock: Lock to protect deadlines */
	spinlock_t lock;
	/** @deadline: Current deadline, protected by @lock */
	ktime_t deadline;
	/** @state: Deadline state, protected by @lock */
	enum xe_deadline_mgr_state state;
};

#endif
