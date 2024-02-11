/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SCALE_BITMAP_TYPES_H
#define __LINUX_SCALE_BITMAP_TYPES_H

#include <linux/cache.h>
#include <linux/types.h>
#include <linux/wait_types.h>

/**
 * struct sbitmap_word - Word in a &struct sbitmap.
 */
struct sbitmap_word {
	/**
	 * @word: word holding free bits
	 */
	unsigned long word;

	/**
	 * @cleared: word holding cleared bits
	 */
	unsigned long cleared ____cacheline_aligned_in_smp;
} ____cacheline_aligned_in_smp;

/**
 * struct sbitmap - Scalable bitmap.
 *
 * A &struct sbitmap is spread over multiple cachelines to avoid ping-pong. This
 * trades off higher memory usage for better scalability.
 */
struct sbitmap {
	/**
	 * @depth: Number of bits used in the whole bitmap.
	 */
	unsigned int depth;

	/**
	 * @shift: log2(number of bits used per word)
	 */
	unsigned int shift;

	/**
	 * @map_nr: Number of words (cachelines) being used for the bitmap.
	 */
	unsigned int map_nr;

	/**
	 * @round_robin: Allocate bits in strict round-robin order.
	 */
	bool round_robin;

	/**
	 * @map: Allocated bitmap.
	 */
	struct sbitmap_word *map;

	/*
	 * @alloc_hint: Cache of last successfully allocated or freed bit.
	 *
	 * This is per-cpu, which allows multiple users to stick to different
	 * cachelines until the map is exhausted.
	 */
	unsigned int __percpu *alloc_hint;
};

/**
 * struct sbq_wait_state - Wait queue in a &struct sbitmap_queue.
 */
struct sbq_wait_state {
	/**
	 * @wait: Wait queue.
	 */
	wait_queue_head_t wait;
} ____cacheline_aligned_in_smp;

/**
 * struct sbitmap_queue - Scalable bitmap with the added ability to wait on free
 * bits.
 *
 * A &struct sbitmap_queue uses multiple wait queues and rolling wakeups to
 * avoid contention on the wait queue spinlock. This ensures that we don't hit a
 * scalability wall when we run out of free bits and have to start putting tasks
 * to sleep.
 */
struct sbitmap_queue {
	/**
	 * @sb: Scalable bitmap.
	 */
	struct sbitmap sb;

	/**
	 * @wake_batch: Number of bits which must be freed before we wake up any
	 * waiters.
	 */
	unsigned int wake_batch;

	/**
	 * @wake_index: Next wait queue in @ws to wake up.
	 */
	atomic_t wake_index;

	/**
	 * @ws: Wait queues.
	 */
	struct sbq_wait_state *ws;

	/*
	 * @ws_active: count of currently active ws waitqueues
	 */
	atomic_t ws_active;

	/**
	 * @min_shallow_depth: The minimum shallow depth which may be passed to
	 * sbitmap_queue_get_shallow()
	 */
	unsigned int min_shallow_depth;

	/**
	 * @completion_cnt: Number of bits cleared passed to the
	 * wakeup function.
	 */
	atomic_t completion_cnt;

	/**
	 * @wakeup_cnt: Number of thread wake ups issued.
	 */
	atomic_t wakeup_cnt;
};

#endif /* __LINUX_SCALE_BITMAP_TYPES_H */
