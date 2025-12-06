/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_HQ_SPINLOCK_SLOWPATH
#error "Do not include this file!"
#endif

#define IRQ_NODE (MAX_NUMNODES + 1)
#define Q_NEW_NODE_QUEUE 1
#define LOCK_ID_BITS		(12)

#define LOCK_ID_MAX			(1 << LOCK_ID_BITS)
#define LOCK_ID_NONE		(LOCK_ID_MAX + 1)

#define _QUEUE_TAIL_MASK (((1ULL << 32) - 1) << 0)
#define _QUEUE_SEQ_COUNTER_MASK (((1ULL << 32) - 1) << 32)

/*
 * Output code for handoff-logic:
 *
 * ==  0 (HQLOCK_HANDOFF_LOCAL) - has local nodes to handoff
 *  >  0 - has remote node to handoff, id is visible
 * == -1 (HQLOCK_HANDOFF_REMOTE_HEAD) - has remote node to handoff,
 *         id isn't visible yet, will be in the *lock->head_node*
 */
enum {
	HQLOCK_HANDOFF_LOCAL = 0,
	HQLOCK_HANDOFF_REMOTE_HEAD = -1
};

typedef enum {
	LOCK_NO_MODE = 0,
	LOCK_MODE_QSPINLOCK = 1,
	LOCK_MODE_HQLOCK = 2,
} hqlock_mode_t;

struct numa_qnode {
	struct mcs_spinlock mcs;

	u16 lock_id;
	u16 wrong_fallback_tail;
	u16 general_handoffs;

	u16 numa_node;
};

struct numa_queue {
	struct numa_qnode *head;
	union {
		u64 seq_counter_tail;
		struct {
			u32 tail;
			u32 seq_counter;
		};
	};

	u16 next_node;
	u16 prev_node;

	u16 handoffs_not_head;
} ____cacheline_aligned;

/**
 * Lock metadata
 * "allocated"/"freed" on demand.
 *
 * Used to dynamically bind numa_queue to a lock,
 * maintain FIFO-order for the NUMA-queues.
 *
 * seq_counter is needed to distinguish metadata usage
 * by different locks, preventing local contenders
 * from queueing in the wrong per-NUMA queue
 *
 * @see set_bucket
 * @see numa_xchg_tail
 */
struct lock_metadata {
	atomic_t seq_counter;
	struct qspinlock *lock_ptr;

	/* NUMA-queues of contenders ae kept in FIFO order */
	union {
		u32 nodes_tail;
		struct {
			u16 tail_node;
			u16 head_node;
		};
	};
};

static inline int decode_lock_mode(u32 lock_val)
{
	return (lock_val & _Q_LOCK_MODE_MASK) >> _Q_LOCK_MODE_OFFSET;
}

static inline u32 encode_lock_mode(u16 lock_id)
{
	if (lock_id == LOCK_ID_NONE)
		return LOCK_MODE_QSPINLOCK << _Q_LOCK_MODE_OFFSET;

	return LOCK_MODE_HQLOCK << _Q_LOCK_MODE_OFFSET;
}

static inline u64 encode_tc(u32 tail, u32 counter)
{
	u64 __tail = (u64)tail;
	u64 __counter = (u64)counter;

	return __tail | (__counter << 32);
}

static inline u32 decode_tc_tail(u64 tail_counter)
{
	return (u32)(tail_counter & _QUEUE_TAIL_MASK);
}

static inline u32 decode_tc_counter(u64 tail_counter)
{
	return (u32)((tail_counter & _QUEUE_SEQ_COUNTER_MASK) >> 32);
}
