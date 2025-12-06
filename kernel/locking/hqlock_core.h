/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_HQ_SPINLOCK_SLOWPATH
#error "Do not include this file!"
#endif

#include <linux/nodemask.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/moduleparam.h>
#include <linux/sched/rt.h>
#include <linux/random.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/panic.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/sprintf.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/swab.h>
#include <linux/hash.h>

/* Contains queues for all possible lock id */
static struct numa_queue *queue_table[MAX_NUMNODES];

#include "hqlock_types.h"
#include "hqlock_proc.h"

/* Gets node_id (1..N) */
static inline struct numa_queue *
get_queue(u16 lock_id, u16 node_id)
{
	return &queue_table[node_id - 1][lock_id];
}

static inline struct numa_queue *
get_local_queue(struct numa_qnode *qnode)
{
	return get_queue(qnode->lock_id, qnode->numa_node);
}

static inline void init_queue_link(struct numa_queue *queue)
{
	queue->prev_node = 0;
	queue->next_node = 0;
}

static inline void init_queue(struct numa_qnode *qnode)
{
	struct numa_queue *queue = get_local_queue(qnode);

	queue->head = qnode;
	queue->handoffs_not_head = 0;
	init_queue_link(queue);
}

static void set_next_queue(u16 lock_id, u16 prev_node_id, u16 node_id)
{
	struct numa_queue *local_queue = get_queue(lock_id, node_id);
	struct numa_queue *prev_queue =
		get_queue(lock_id, prev_node_id);

	WRITE_ONCE(local_queue->prev_node, prev_node_id);
	/*
	 * Needs to be guaranteed the following:
	 * when appending "local_queue", if "prev_queue->next_node" link
	 * is observed then "local_queue->prev_node" is also observed.
	 *
	 * We need this to guarantee correctness of concurrent
	 * "unlink_node_queue" for the "prev_queue", if "prev_queue" is the first in the list.
	 * [prev_queue] <-> [local_queue]
	 *
	 * In this case "unlink_node_queue" would be setting "local_queue->prev_node = 0", thus
	 * w/o the smp-barrier, it might race with "set_next_queue", if
	 * "local_queue->prev_node = prev_node_id" happens afterwards, leading to corrupted list.
	 */
	smp_wmb();
	WRITE_ONCE(prev_queue->next_node, node_id);
}

static inline struct lock_metadata *get_meta(u16 lock_id);

/**
 * Put new node's queue into global NUMA-level queue
 */
static inline u16 append_node_queue(u16 lock_id, u16 node_id)
{
	struct lock_metadata *lock_meta = get_meta(lock_id);
	u16 prev_node_id = xchg(&lock_meta->tail_node, node_id);

	if (prev_node_id)
		set_next_queue(lock_id, prev_node_id, node_id);
	else
		WRITE_ONCE(lock_meta->head_node, node_id);
	return prev_node_id;
}

#include "hqlock_meta.h"

/**
 * Update tail
 *
 * Call proper function depending on lock's mode
 * until successful queuing
 */
static inline u32 hqlock_xchg_tail(struct qspinlock *lock, u32 tail,
				 struct mcs_spinlock *node, bool *numa_awareness_on)
{
	struct numa_qnode *qnode = (struct numa_qnode *)node;

	u16 lock_id;
	u32 old_tail;
	u32 next_tail = tail;

	/*
	 * Key lock's mode switches questions:
	 * - After init lock is in LOCK_MODE_QSPINLOCK
	 * - If many contenders have come while lock was in LOCK_MODE_QSPINLOCK,
	 *   we want this lock to use NUMA awareness next time,
	 *   so we clean LOCK_MODE_QSPINLOCK, see 'low_contention_try_clear_tail'
	 * - During next lock's usages we try to go through NUMA-aware path.
	 *   We can fail here, because we use shared metadata
	 *   and can have a conflict with another lock, see 'hqlock_meta.h' for details.
	 *   In this case we fallback to generic qspinlock approach.
	 *
	 * In other words, lock can be in 3 mode states:
	 *
	 * 1. LOCK_MODE_QSPINLOCK - there was low contention or not at all earlier,
	 *    or (unlikely) a conflict in metadata
	 * 2. LOCK_NO_MODE - there was a contention on a lock earlier,
	 *    now there are no contenders in the queue (we are likely the first)
	 *    and we need to try using NUMA awareness
	 * 3. LOCK_MODE_HQLOCK - lock is currently under contention
	 *    and using NUMA awareness.
	 */

	/*
	 * numa_awareness_on == false means we saw LOCK_MODE_QSPINLOCK (1st state)
	 * before starting slowpath, see 'queued_spin_lock_slowpath'
	 */
	if (*numa_awareness_on == false &&
		try_update_tail_qspinlock_mode(lock, tail, &old_tail, &next_tail))
		return old_tail;

	/* Calculate the lock_id hash here once */
	qnode->lock_id = lock_id = hash_ptr(lock, LOCK_ID_BITS);

try_again:
	/*
	 * Lock is in state 2 or 3 - go through NUMA-aware path
	 */
	if (try_update_tail_hqlock_mode(lock, lock_id, qnode, tail, &next_tail, &old_tail)) {
		*numa_awareness_on = true;
		return old_tail;
	}

	/*
	 * We have failed (conflict in metadata), now lock is in LOCK_MODE_QSPINLOCK again
	 */
	if (try_update_tail_qspinlock_mode(lock, tail, &old_tail, &next_tail)) {
		*numa_awareness_on = false;
		return old_tail;
	}

	/*
	 * We were slow and clear_tail after high contention has already happened
	 * (very unlikely situation)
	 */
	goto try_again;
}

static inline void hqlock_clear_pending(struct qspinlock *lock, u32 old_val)
{
	WRITE_ONCE(lock->pending, (old_val & _Q_LOCK_TYPE_MODE_MASK) >> _Q_PENDING_OFFSET);
}

static inline void hqlock_clear_pending_set_locked(struct qspinlock *lock, u32 old_val)
{
	WRITE_ONCE(lock->locked_pending,
			_Q_LOCKED_VAL | (old_val & _Q_LOCK_TYPE_MODE_MASK));
}

static inline void unlink_node_queue(u16 lock_id,
					u16 prev_node_id,
					u16 next_node_id)
{
	struct numa_queue *prev_queue =
		prev_node_id ? get_queue(lock_id, prev_node_id) : NULL;
	struct numa_queue *next_queue = get_queue(lock_id, next_node_id);

	if (prev_queue)
		WRITE_ONCE(prev_queue->next_node, next_node_id);
	/*
	 * This is guaranteed to be ordered "after" next_node_id observation
	 * by implicit full-barrier in the caller-code.
	 */
	WRITE_ONCE(next_queue->prev_node, prev_node_id);
}

static inline bool try_clear_queue_tail(struct numa_queue *queue, u32 tail)
{
	/*
	 * We need full ordering here to:
	 * - ensure all prior operations with global tail and prev_queue
	 *   are observed before clearing local tail
	 * - guarantee all subsequent operations
	 *   with metadata release, unlink etc will be observed after clearing local tail
	 */
	return cmpxchg(&queue->tail, tail, 0) == tail;
}

/*
 * Determine if we have another local and global contenders.
 * Try clear local and global tail, understand handoff type we need to perform.
 * In case we are the last, free lock's metadata
 */
static inline bool hqlock_try_clear_tail(struct qspinlock *lock, u32 val,
				       u32 tail, struct mcs_spinlock *node,
				       int *p_next_node)
{
	bool ret = false;
	struct numa_qnode *qnode = (void *)node;

	u16 lock_id = qnode->lock_id;
	u16 local_node = qnode->numa_node;
	struct numa_queue *queue = get_queue(lock_id, qnode->numa_node);

	struct lock_metadata *lock_meta = get_meta(lock_id);

	u16 prev_node = 0, next_node = 0;
	u16 node_tail;

	u32 old_val;

	bool lock_tail_updated = false;
	bool lock_tail_cleared = false;

	/* Do we have *next node* arrived */
	bool pending_next_node = false;

	tail >>= _Q_TAIL_OFFSET;

	/* Do we have other CPUs in the node queue ? */
	if (READ_ONCE(queue->tail) != tail) {
		*p_next_node = HQLOCK_HANDOFF_LOCAL;
		goto out;
	}

	/*
	 * Key observations and actions:
	 * 1) next queue isn't observed:
	 *    a) if prev queue is observed, try to unpublish local queue
	 *    b) if prev queue is not observed, try to clean global tail
	 *    Anyway, perform these operations before clearing local tail.
	 *
	 *    Such trick is essential to safely unlink the local queue,
	 *    otherwise we could race with upcomming local contenders,
	 *    which will perform 'append_node_queue' while our unlink is not properly done.
	 *
	 * 2) next queue is observed:
	 *    safely perform 'try_clear_queue_tail' and unlink local node if succeeded.
	 */

	prev_node = READ_ONCE(queue->prev_node);
	pending_next_node = READ_ONCE(lock_meta->tail_node) != local_node;

	/*
	 * Tail case:
	 * [prev_node] -> [local_node], lock->tail_node == local_node
	 *
	 * There're no nodes after us at the moment, try updating the "lock->tail_node"
	 */
	if (!pending_next_node && prev_node) {
		struct numa_queue *prev_queue =
			get_queue(lock_id, prev_node);

		/* Reset next_node, in case no one will come after */
		WRITE_ONCE(prev_queue->next_node, 0);

		/*
		 * release to publish prev_queue->next_node = 0
		 * and to ensure ordering with 'READ_ONCE(queue->tail) != tail'
		 */
		if (cmpxchg_release(&lock_meta->tail_node, local_node, prev_node) == local_node) {
			lock_tail_updated = true;

			queue->next_node = 0;
			queue->prev_node = 0;
			next_node = 0;
		} else {
			/* If some node came after the local meanwhile, reset next_node back */
			WRITE_ONCE(prev_queue->next_node, local_node);

			/* We either observing updated "queue->next" or it equals zero */
			next_node = READ_ONCE(queue->next_node);
		}
	}

	node_tail = READ_ONCE(lock_meta->tail_node);

	/* If nobody else is waiting, try clean global tail */
	if (node_tail == local_node && !prev_node) {
		old_val = (((u32)local_node) | (((u32)local_node) << 16));
		/* release to ensure ordering with 'READ_ONCE(queue->tail) != tail' */
		lock_tail_cleared = try_cmpxchg_release(&lock_meta->nodes_tail, &old_val, 0);
	}

	/*
	 * lock->tail_node was not updated and cleared,
	 * so we have at least single non-empty node after us
	 */
	if (!lock_tail_updated && !lock_tail_cleared) {
		/*
		 * If there's a node came before clearing node queue - wait for it to link properly.
		 * We need this for correct upcoming *unlink*, otherwise the *unlink* might race with parallel set_next_node()
		 */
		if (!next_node) {
			next_node =
				smp_cond_load_relaxed(&queue->next_node, (VAL));
		}
	}

	/* if we're the last one in the queue - clear the queue tail */
	if (try_clear_queue_tail(queue, tail)) {
		/*
		 * "lock_tail_cleared == true"
		 * It means: we cleared "lock->tail_node" and "lock->head_node".
		 *
		 * First new contender will do "global spin" anyway, so no handoff needed
		 * "ret == true"
		 */
		if (lock_tail_cleared) {
			ret = true;

			/*
			 * If someone has arrived in the meanwhile,
			 * don't try to free the metadata.
			 */
			old_val = READ_ONCE(lock_meta->nodes_tail);
			if (!old_val) {
				/*
				 * We are probably the last contender,
				 * so, need to free lock's metadata.
				 */
				release_lock_meta(lock, lock_meta, qnode);
			}
			goto out;
		}

		/*
		 * "lock_tail_updated == true" (implies "lock_tail_cleared == false")
		 * It means we have at least "prev_node" and unlinked "local node"
		 *
		 * As we unlinked "local node", we only need to guarantee correct
		 * remote handoff, thus we have:
		 * "ret == false"
		 * "next_node == HQLOCK_HANDOFF_REMOTE_HEAD"
		 */
		if (lock_tail_updated) {
			*p_next_node = HQLOCK_HANDOFF_REMOTE_HEAD;
			goto out;
		}

		/*
		 * "!lock_tail_cleared && !lock_tail_updated"
		 * It means we have at least single node after us.
		 *
		 * remote handoff and corect "local node" unlink are needed.
		 *
		 * "next_node" visibility guarantees that we observe
		 * correctly additon of "next_node", so the following unlink
		 * is safe and correct.
		 *
		 * "next_node > 0"
		 * "ret == false"
		 */
		unlink_node_queue(lock_id, prev_node, next_node);

		/*
		 * If at the head - update one.
		 *
		 * Another place, where "lock->head_node" is updated is "append_node_queue"
		 * But we're safe, as that happens only with the first node on empty "node list".
		 */
		if (!prev_node)
			WRITE_ONCE(lock_meta->head_node, next_node);

		*p_next_node = next_node;
	} else {
		/*
		 * local queue has other contenders.
		 *
		 * 1) "lock_tail_updated == true":
		 * It means we have at least "prev_node" and unlinked "local node"
		 * Also, some new nodes can arrive and link after "prev_node".
		 * We should just re-add "local node": (prev_node) => ... => (local_node)
		 * and perform local handoff, as other CPUs from the local node do "mcs spin"
		 *
		 * 2) "lock_tail_cleared == true"
		 * It means we cleared "lock->tail_node" and "lock->head_node".
		 * We need to re-add "local node" and move "local_queue->head" to the next "mcs-node",
		 * which is in the progress of linking after the current "mcs-node"
		 * (that's why we couldn't clear the "local_queue->tail").
		 *
		 * Meanwhile other nodes can arrive: (new_node) => (...)
		 * That "new_node" will spin in "global spin" mode.
		 * In this case no handoff needed.
		 *
		 * 3) "!lock_tail_cleared && !lock_tail_updated"
		 * It means we had at least one node after us before 'try_clear_queue_tail'
		 * and only need to perform local handoff
		 */

		/* Cases 1) and 2) */
		if (lock_tail_updated || lock_tail_cleared) {
			u16 prev_node_id;

			init_queue_link(queue);
			prev_node_id =
				append_node_queue(lock_id, local_node);

			if (prev_node_id && lock_tail_cleared) {
				/* Case 2) */
				ret = true;
				WRITE_ONCE(queue->head,
					   (void *) smp_cond_load_relaxed(&node->next, (VAL)));
				goto out;
			}
		}

		/* Cases 1) and 3) */
		*p_next_node = HQLOCK_HANDOFF_LOCAL;
		ret = false;
	}
out:
	/*
	 * Either handoff for current node,
	 * or remote handoff if the quota is expired
	 */
	return ret;
}

static inline void hqlock_handoff(struct qspinlock *lock,
					 struct mcs_spinlock *node,
					 struct mcs_spinlock *next, u32 tail,
					 int handoff_info);


/*
 * Chech if contention has risen and if we need to set NUMA-aware mode
 */
static __always_inline bool determine_contention_qspinlock_mode(struct mcs_spinlock *node)
{
	struct numa_qnode *qnode = (void *)node;

	if (qnode->general_handoffs > READ_ONCE(hqlock_general_handoffs_turn_numa))
		return true;
	return false;
}

static __always_inline bool low_contention_try_clear_tail(struct qspinlock *lock,
					     u32 val,
					     struct mcs_spinlock *node)
{
	u32 update_val = _Q_LOCKED_VAL | _Q_LOCKTYPE_HQ;

	bool high_contention = determine_contention_qspinlock_mode(node);

	/*
	 * If we have high contention, we set _Q_LOCK_INVALID_TAIL
	 * to notify upcomming contenders, which have seen QSPINLOCK mode,
	 * that performing generic 'xchg_tail' is wrong.
	 *
	 * We cannot also set HQLOCK mode here,
	 * because first contender in updated mode
	 * should check if lock's metadata is free
	 */
	if (!high_contention)
		update_val |= _Q_LOCK_MODE_QSPINLOCK_VAL;
	else
		update_val |= _Q_LOCK_INVALID_TAIL;

	return atomic_try_cmpxchg_relaxed(&lock->val, &val, update_val);
}

static __always_inline void low_contention_mcs_lock_handoff(struct mcs_spinlock *node,
					       struct mcs_spinlock *next, struct mcs_spinlock *prev)
{
	struct numa_qnode *qnode = (void *)node;
	struct numa_qnode *qnext = (void *)next;

	static u16 max_u16 = (u16)(-1);

	u16 general_handoffs = qnode->general_handoffs;

	if (next != prev && likely(general_handoffs + 1 != max_u16))
		general_handoffs++;

#ifdef CONFIG_HQSPINLOCKS_DEBUG
	if (READ_ONCE(max_general_handoffs) < general_handoffs)
		WRITE_ONCE(max_general_handoffs, general_handoffs);
#endif
	qnext->general_handoffs = general_handoffs;
	arch_mcs_spin_unlock_contended(&next->locked);
}

static inline void hqlock_clear_tail_handoff(struct qspinlock *lock, u32 val,
				    u32 tail,
				    struct mcs_spinlock *node,
				    struct mcs_spinlock *next,
					struct mcs_spinlock *prev,
					bool is_numa_lock)
{
	int handoff_info;
	struct numa_qnode *qnode = (void *)node;

	/*
	 * qnode->wrong_fallback_tail means we have queued globally
	 * in 'try_update_tail_qspinlock_mode' after another contender,
	 * but lock's mode was not QSPINLOCK in that moment.
	 *
	 * First confused contender has restored _Q_LOCK_INVALID_TAIL in global tail
	 * and set us in his local queue.
	 */
	if (is_numa_lock || qnode->wrong_fallback_tail) {
		/*
		 * Because of splitting generic tail and NUMA tail we must set locked before clearing tail,
		 * otherwise double lock is possible
		 */
		set_locked(lock);

		if (hqlock_try_clear_tail(lock, val, tail, node, &handoff_info))
			return;

		hqlock_handoff(lock, node, next, tail, handoff_info);
	} else {
		if ((val & _Q_TAIL_MASK) == tail) {
			if (low_contention_try_clear_tail(lock, val, node))
				return;
		}

		set_locked(lock);

		if (!next)
			next = smp_cond_load_relaxed(&node->next, (VAL));

		low_contention_mcs_lock_handoff(node, next, prev);
	}
}

static inline void hqlock_init_node(struct mcs_spinlock *node)
{
	struct numa_qnode *qnode = (void *)node;

	qnode->general_handoffs = 0;
	qnode->numa_node = numa_node_id() + 1;
	qnode->lock_id = 0;
	qnode->wrong_fallback_tail = 0;
}

static inline void reset_handoff_counter(struct numa_qnode *qnode)
{
	qnode->general_handoffs = 0;
}

static inline void handoff_local(struct mcs_spinlock *node,
					       struct mcs_spinlock *next,
					       u32 tail)
{
	static u16 max_u16 = (u16)(-1);

	struct numa_qnode *qnode = (struct numa_qnode *)node;
	struct numa_qnode *qnext = (struct numa_qnode *)next;

	u16 general_handoffs = qnode->general_handoffs;

	if (likely(general_handoffs + 1 != max_u16))
		general_handoffs++;

	qnext->general_handoffs = general_handoffs;

	u16 wrong_fallback_tail = qnode->wrong_fallback_tail;

	if (wrong_fallback_tail != 0 && wrong_fallback_tail != (tail >> _Q_TAIL_OFFSET)) {
		qnext->numa_node = qnode->numa_node;
		qnext->wrong_fallback_tail = wrong_fallback_tail;
		qnext->lock_id = qnode->lock_id;
	}

	arch_mcs_spin_unlock_contended(&next->locked);
}

static inline void handoff_remote(struct qspinlock *lock,
						struct numa_qnode *qnode,
						u32 tail, int handoff_info)
{
	struct numa_queue *next_queue = NULL;
	struct mcs_spinlock *mcs_head = NULL;
	struct numa_qnode *qhead = NULL;
	u16 lock_id = qnode->lock_id;

	struct lock_metadata *lock_meta = get_meta(lock_id);
	struct numa_queue *queue = get_local_queue(qnode);

	u16 next_node_id;
	u16 node_head, node_tail;

	node_tail = READ_ONCE(lock_meta->tail_node);
	node_head = READ_ONCE(lock_meta->head_node);

	/*
	 * 'handoffs_not_head > 0' means at the head of NUMA-level queue we have a node
	 * which is heavily loaded and has performed a remote handoff upon reaching the threshold.
	 *
	 * Perform handoff to the head instead of next node in the NUMA-level queue,
	 * if handoffs_not_head >= nr_online_nodes
	 * (It means other contended nodes have been taking the lock at least once after the head one)
	 */
	u16 handoffs_not_head = READ_ONCE(queue->handoffs_not_head);

	if (handoff_info > 0 && (handoffs_not_head < nr_online_nodes)) {
		next_node_id = handoff_info;
		if (node_head != qnode->numa_node)
			handoffs_not_head++;
	} else {
		if (!node_head) {
			/* If we're here - we have defintely other node-contenders, let's wait */
			next_node_id = smp_cond_load_relaxed(&lock_meta->head_node, (VAL));
		} else {
			next_node_id = node_head;
		}

		handoffs_not_head = 0;
	}

	next_queue = get_queue(lock_id, next_node_id);
	WRITE_ONCE(next_queue->handoffs_not_head, handoffs_not_head);

	qhead = READ_ONCE(next_queue->head);

	mcs_head = (void *) qhead;

	/* arch_mcs_spin_unlock_contended implies smp-barrier */
	arch_mcs_spin_unlock_contended(&mcs_head->locked);
}

static inline bool has_other_nodes(struct qspinlock *lock,
				   struct numa_qnode *qnode)
{
	struct lock_metadata *lock_meta = get_meta(qnode->lock_id);

	return lock_meta->tail_node != qnode->numa_node;
}

static inline bool is_node_threshold_reached(struct numa_qnode *qnode)
{
	return qnode->general_handoffs > hqlock_fairness_threshold;
}

static inline void hqlock_handoff(struct qspinlock *lock,
					 struct mcs_spinlock *node,
					 struct mcs_spinlock *next, u32 tail,
					 int handoff_info)
{
	struct numa_qnode *qnode = (void *)node;
	u16 lock_id = qnode->lock_id;
	struct lock_metadata *lock_meta = get_meta(lock_id);
	struct numa_queue *queue = get_local_queue(qnode);

	if (handoff_info == HQLOCK_HANDOFF_LOCAL) {
		if (!next)
			next = smp_cond_load_relaxed(&node->next, (VAL));
		WRITE_ONCE(queue->head, (void *) next);

		bool threshold_expired = is_node_threshold_reached(qnode);

		if (!threshold_expired || qnode->wrong_fallback_tail) {
			handoff_local(node, next, tail);
			return;
		}

		u16 queue_next = READ_ONCE(queue->next_node);
		bool has_others = has_other_nodes(lock, qnode);

		/*
		 * This check is racy, but it's ok,
		 * because we fallback to local node in the worst case
		 * and do not call reset_handoff_counter.
		 * Next local contender will perform remote handoff
		 * after next queue is properly linked
		 */
		if (has_others) {
			handoff_info =
				queue_next > 0 ? queue_next : HQLOCK_HANDOFF_LOCAL;
		} else {
			handoff_info = HQLOCK_HANDOFF_REMOTE_HEAD;
		}

		if (handoff_info == HQLOCK_HANDOFF_LOCAL ||
			(handoff_info == HQLOCK_HANDOFF_REMOTE_HEAD &&
				READ_ONCE(lock_meta->head_node) == qnode->numa_node)) {
			/*
			 * No other nodes have come yet, so we can clean fairness counter
			 */
			if (handoff_info == HQLOCK_HANDOFF_REMOTE_HEAD)
				reset_handoff_counter(qnode);
			handoff_local(node, next, tail);
			return;
		}
	}

	handoff_remote(lock, qnode, tail, handoff_info);
	reset_handoff_counter(qnode);
}

static void __init hqlock_alloc_global_queues(void)
{
	int nid;
	phys_addr_t phys_addr;

	// meta_pool
	unsigned long meta_pool_size =
		sizeof(struct lock_metadata) * LOCK_ID_MAX;

	pr_info("Init HQspinlock lock_metadata info: size = %lu B\n",
		meta_pool_size);

	phys_addr = memblock_alloc_range_nid(
				meta_pool_size, L1_CACHE_BYTES, 0,
				MEMBLOCK_ALLOC_ACCESSIBLE, NUMA_NO_NODE, false);

	if (!phys_addr)
		panic("HQspinlock lock_metadata metadata info: allocation failure.\n");

	meta_pool = phys_to_virt(phys_addr);
	memset(meta_pool, 0, meta_pool_size);

	for (int i = 0; i < LOCK_ID_MAX; i++)
		atomic_set(&meta_pool[i].seq_counter, 0);

	/* Total queues size for all buckets */
	unsigned long queues_size =
		LOCK_ID_MAX *
		ALIGN(sizeof(struct numa_queue), L1_CACHE_BYTES);

	pr_info("Init HQspinlock per-NUMA metadata (per-node size = %lu B)\n",
		queues_size);


	for_each_node(nid) {
		phys_addr = memblock_alloc_range_nid(
			queues_size, L1_CACHE_BYTES, 0, MEMBLOCK_ALLOC_ACCESSIBLE,
			nid, false);

		if (!phys_addr)
			panic("HQspinlock per-NUMA metadata: allocation failure for node %d.\n", nid);

		queue_table[nid] = phys_to_virt(phys_addr);
		memset(queue_table[nid], 0, queues_size);
	}
}

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#define hq_queued_spin_lock_slowpath	pv_ops.lock.queued_spin_lock_slowpath
#else
void (*hq_queued_spin_lock_slowpath)(struct qspinlock *lock, u32 val) =
		native_queued_spin_lock_slowpath;
EXPORT_SYMBOL(hq_queued_spin_lock_slowpath);
#endif

static int numa_spinlock_flag;

static int __init numa_spinlock_setup(char *str)
{
	if (!strcmp(str, "auto")) {
		numa_spinlock_flag = 0;
		return 1;
	} else if (!strcmp(str, "on")) {
		numa_spinlock_flag = 1;
		return 1;
	} else if (!strcmp(str, "off")) {
		numa_spinlock_flag = -1;
		return 1;
	}

	return 0;
}
__setup("numa_spinlock=", numa_spinlock_setup);

void __hq_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);

void __init hq_configure_spin_lock_slowpath(void)
{
	if (numa_spinlock_flag < 0)
		return;

	if (numa_spinlock_flag == 0 && (nr_node_ids < 2 ||
		    hq_queued_spin_lock_slowpath !=
			native_queued_spin_lock_slowpath)) {
		return;
	}

	numa_spinlock_flag = 1;
	hq_queued_spin_lock_slowpath = __hq_queued_spin_lock_slowpath;
	pr_info("Enabling HQspinlock\n");
	hqlock_alloc_global_queues();
}
