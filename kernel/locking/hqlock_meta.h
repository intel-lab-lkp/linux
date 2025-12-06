/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_HQ_SPINLOCK_SLOWPATH
#error "Do not include this file!"
#endif

/* Lock metadata pool */
static struct lock_metadata *meta_pool;

static inline struct lock_metadata *get_meta(u16 lock_id)
{
	return &meta_pool[lock_id];
}

static inline hqlock_mode_t set_lock_mode(struct qspinlock *lock, int __val, u16 lock_id)
{
	u32 val = (u32)__val;
	u32 new_val = 0;
	u32 lock_mode = encode_lock_mode(lock_id);

	while (!(val & _Q_LOCK_MODE_MASK)) {
		/*
		 * We need wait until pending is gone.
		 * Otherwise, clearing pending can erase a NUMA mode we will set here
		 */
		if (val & _Q_PENDING_VAL) {
			val = atomic_cond_read_relaxed(&lock->val, !(VAL & _Q_PENDING_VAL));

			if (val & _Q_LOCK_MODE_MASK)
				return LOCK_NO_MODE;
		}

		/*
		 * If we are enabling NUMA-awareness, we should keep previous value in lock->tail
		 * in case of having contenders seen LOCK_MODE_QSPINLOCK and set their tails via xchg_tail
		 * (They will restore it to _Q_LOCK_INVALID_TAIL later).
		 * If we are setting LOCK_MODE_QSPINLOCK, remove _Q_LOCK_INVALID_TAIL
		 */
		if (lock_id != LOCK_ID_NONE)
			new_val = val | lock_mode;
		else
			new_val = (val & ~_Q_LOCK_INVALID_TAIL) | lock_mode;

		/*
		 * If we're setting LOCK_MODE_HQLOCK, make sure all "seq_counter"
		 * updates (per-queue, lock_meta) are observed before lock mode update.
		 * Paired with smp_rmb() in setup_lock_mode().
		 */
		if (lock_id != LOCK_ID_NONE)
			smp_wmb();

		bool updated = atomic_try_cmpxchg_relaxed(&lock->val, &val, new_val);

		if (updated) {
			return (lock_id == LOCK_ID_NONE) ?
				LOCK_MODE_QSPINLOCK : LOCK_MODE_HQLOCK;
		}
	}

	return LOCK_NO_MODE;
}

static inline hqlock_mode_t set_mode_hqlock(struct qspinlock *lock, int val, u16 lock_id)
{
	return set_lock_mode(lock, val, lock_id);
}

static inline hqlock_mode_t set_mode_qspinlock(struct qspinlock *lock, int val)
{
	return set_lock_mode(lock, val, LOCK_ID_NONE);
}

/* Dynamic lock-mode conditions */
static inline bool is_mode_hqlock(int val)
{
	return decode_lock_mode(val) == LOCK_MODE_HQLOCK;
}

static inline bool is_mode_qspinlock(int val)
{
	return decode_lock_mode(val) == LOCK_MODE_QSPINLOCK;
}

enum meta_status {
	META_CONFLICT = 0,
	META_GRABBED,
	META_SHARED,
};

static inline enum meta_status grab_lock_meta(struct qspinlock *lock, u32 lock_id, u32 *seq)
{
	int nid, seq_counter;
	struct qspinlock *old = READ_ONCE(meta_pool[lock_id].lock_ptr);

	if (old && old != lock)
		return META_CONFLICT;

	if (old && old == lock)
		return META_SHARED;

	old = cmpxchg_acquire(&meta_pool[lock_id].lock_ptr, NULL, lock);
	if (!old)
		goto init_meta;

	/* Hash-conflict */
	if (old != lock)
		return META_CONFLICT;

	return META_SHARED;
init_meta:
	/*
	 * Update allocations counter and set it to per-NUMA queues
	 * to prevent upcomming contenders from parking on deallocated queues
	 */
	seq_counter = atomic_inc_return_relaxed(&meta_pool[lock_id].seq_counter);

	/* Very unlikely we can overflow */
	if (unlikely(seq_counter == 0))
		seq_counter = atomic_inc_return_relaxed(&meta_pool[lock_id].seq_counter);

	for_each_online_node(nid) {
		struct numa_queue *queue = &queue_table[nid][lock_id];
		WRITE_ONCE(queue->seq_counter, (u32)seq_counter);
	}

	*seq = seq_counter;
#ifdef CONFIG_HQSPINLOCKS_DEBUG
	int current_used = atomic_inc_return_relaxed(&cur_buckets_in_use);

	if (READ_ONCE(max_buckets_in_use) < current_used)
		WRITE_ONCE(max_buckets_in_use, current_used);
#endif
	return META_GRABBED;
}

/*
 * Try to setup current lock mode:
 *
 * LOCK_MODE_HQLOCK or fallback to default LOCK_MODE_QSPINLOCK
 * if there's hash conflict with another lock in the system.
 *
 * In general the setup consists of grabbing lock-related metadata and
 * publishing the mode in the global lock variable.
 *
 * For quick meta-lookup the pointer hashing is used.
 *
 * To identify "occupied/free" metadata record, we use "meta->lock_ptr"
 * which is set to corresponding spinlock lock pointer or "NULL".
 *
 * The action sequence from initial state is the following:
 *
 * "Find lock-meta by hash" => "Occupy lock-meta" => publish "LOCK_MODE_HQLOCK" in
 * global lock variable.
 *
 */
static inline
hqlock_mode_t setup_lock_mode(struct qspinlock *lock, u16 lock_id, u32 *meta_seq_counter)
{
	hqlock_mode_t mode;

	do {
		enum meta_status status;
		int val = atomic_read(&lock->val);

		if (is_mode_hqlock(val)) {
			struct lock_metadata *lock_meta = get_meta(lock_id);
			/*
			 * The lock is currently in LOCK_MODE_HQLOCK, we need to make sure the
			 * associated metadata isn't used by another lock.
			 *
			 * In the meanwhile several situations can occur:
			 *
			 * [Case 1] Another lock using the meta (hash-conflict)
			 *
			 * If "release + reallocate" of the meta happenned in the meanwhile,
			 * we're guaranteed to observe lock-mode change in the "lock->val",
			 * due to the following event ordering:
			 *
			 * [release_lock_meta]
			 *	Clear lock mode in "lock->val", so we wouldn't
			 *	observe LOCK_MODE_HQLOCK mode.
			 *	=>
			 *        [setup_lock_mode]
			 *	    Update lock->seq_counter
			 *
			 * [Case 2] For exact same lock, some contender did "release + reallocate" of the meta
			 *
			 * Either We'll get newly set "seq_counter", or in the worst case, we'll get
			 * outdated "seq_counter" fail in the CAS(queue) in the caller function.
			 *
			 * [Case 3] Meta is free, nobody using it
			 * [Case 4] The lock mode is changed to LOCK_MODE_QSPINLOCK.
			 */
			int seq_counter = atomic_read(&lock_meta->seq_counter);

			/*
			 * "seq_counter" and "lock->val" should be read in program order.
			 * Otherwise we might observe "seq_counter" updated on-behalf another lock.
			 * Paired with smp_wmb() in set_lock_mode().
			 */
			smp_rmb();
			val = atomic_read(&lock->val);

			if (is_mode_hqlock(val)) {
				*meta_seq_counter = (u32)seq_counter;
				return LOCK_MODE_HQLOCK;
			}
			/*
			 * [else] Here it can be 2 options:
			 *
			 * 1. Lock-meta is free, and nobody using it.
			 *    In this case, we need to try occupying the meta and
			 *    publish lock-mode LOCK_MODE_HQLOCK again.
			 *
			 * 2. Lock mode transitioned to LOCK_MODE_QSPINLOCK mode.
			 */
			continue;
		} else if (is_mode_qspinlock(val)) {
			return LOCK_MODE_QSPINLOCK;
		}

		/*
		 * Trying to get temporary metadata "weak" ownership,
		 * Three situations might happen:
		 *
		 * 1. Metadata isn't used by anyone
		 *    Just take the ownership.
		 *
		 * 2. Metadata is already grabbed by one of the lock contenders.
		 *
		 * 3. Hash conflict: metadata is owned by another lock
		 *    Give up, fallback to LOCK_MODE_QSPINLOCK.
		 */
		status = grab_lock_meta(lock, lock_id, meta_seq_counter);
		if (status == META_SHARED) {
			/*
			 * Someone started publishing lock_id for us:
			 * 1. We can catch the "LOCK_MODE_HQLOCK" mode quickly
			 * 2. We can loop several times before we'll see "LOCK_MODE_HQLOCK" mode set.
			 * (lightweight check)
			 * 3. Another contender might be able to relase lock meta meanwhile.
			 * Either we catch it in above "seq_counter" check, or we'll grab
			 * lock meta first and try publishing lock_id.
			 */
			continue;
		}

		/* Setup the lock-mode */
		if (status == META_GRABBED)
			mode = set_mode_hqlock(lock, val, lock_id);
		else if (status == META_CONFLICT)
			mode = set_mode_qspinlock(lock, val);
		else
			BUG_ON(1);
		/*
		 * If we grabbed the meta but were unable to publish LOCK_MODE_HQLOCK
		 * release it, just by resetting the pointer.
		 */
		if (status == META_GRABBED && mode != LOCK_MODE_HQLOCK) {
			smp_store_release(&meta_pool[lock_id].lock_ptr, NULL);
#ifdef CONFIG_HQSPINLOCKS_DEBUG
			atomic_dec(&cur_buckets_in_use);
#endif
		}
	} while (mode == LOCK_NO_MODE);

	return mode;
}

static inline void release_lock_meta(struct qspinlock *lock,
					struct lock_metadata *meta,
				     struct numa_qnode *qnode)
{
	int nid;
	bool cleared = false;
	u32 upd_val = _Q_LOCKTYPE_HQ | _Q_LOCKED_VAL;
	u16 lock_id = qnode->lock_id;
	int seq_counter = atomic_read(&meta->seq_counter);

	/*
	 * Firstly, go across per-NUMA queues and set seq counter to 0,
	 * it will prevent possible contenders, which haven't even queued locally,
	 * from using already deoccupied metadata.
	 *
	 * We need to perform counter reset with CAS,
	 * because local contenders (we didn't see them while try_clear_lock_tail and try_clear_queue_tail)
	 * may have appeared while we were coming that point.
	 *
	 * If any CAS is not successful, it means someone has already queued locally,
	 * in that case we should restore usability of all local queues
	 * and return seq counter to every per-NUMA queue.
	 *
	 * If all CASes are successful, nobody will queue on this metadata's queues,
	 * and we can free it and allow other locks to use it.
	 */

	/*
	 * Before metadata release read every queue tail,
	 * if we have at least one contender, don't do CASes and leave
	 * (Reads are much faster and also prefetch local queue's cachelines)
	 */
	for_each_online_node(nid) {
		struct numa_queue *queue = get_queue(lock_id, nid + 1);

		if (READ_ONCE(queue->tail) != 0)
			return;
	}

	for_each_online_node(nid) {
		struct numa_queue *queue = get_queue(lock_id, nid + 1);

		if (cmpxchg_relaxed(&queue->seq_counter_tail, encode_tc(0, seq_counter), 0)
			!= encode_tc(0, seq_counter))
			/* Some contender arrived - rollback */
			goto do_rollback;
	}

#ifdef CONFIG_HQSPINLOCKS_DEBUG
	atomic_dec(&cur_buckets_in_use);
#endif
	/*
	 * We need wait until pending is gone.
	 * Otherwise, clearing pending can erase a mode we will set here
	 */
	while (!cleared) {
		u32 old_lock_val = atomic_cond_read_relaxed(&lock->val, !(VAL & _Q_PENDING_VAL));

		cleared = atomic_try_cmpxchg_relaxed(&lock->val,
				&old_lock_val, upd_val | (old_lock_val & _Q_TAIL_MASK));
	}

	/*
	 * guarantee current seq counter is erased from every local queue
	 * and lock mode has been updated before another lock can use metadata
	 */
	smp_store_release(&meta_pool[qnode->lock_id].lock_ptr, NULL);
	return;

do_rollback:
	for_each_online_node(nid) {
		struct numa_queue *queue = get_queue(lock_id, nid + 1);
		WRITE_ONCE(queue->seq_counter, seq_counter);
	}
}

/*
 * Call it if we observe LOCK_MODE_QSPINLOCK.
 *
 * We can do generic xchg_tail in this case,
 * if lock's mode has already been changed, we will get _Q_LOCK_INVALID_TAIL.
 *
 * If we have such a situation, we perform CAS cycle
 * to restore _Q_LOCK_INVALID_TAIL or wait until lock's mode is LOCK_MODE_QSPINLOCK.
 *
 * All upcomming confused contenders will see valid tail.
 * We will remember the last one before successful CAS and put its tail in local queue.
 * During handoff we will notify them about mode change via qnext->wrong_fallback_tail
 */
static inline bool try_update_tail_qspinlock_mode(struct qspinlock *lock, u32 tail, u32 *old_tail, u32 *next_tail)
{
	/*
	 * next_tail may be tail or last cpu from previous unsuccessful call
	 * (highly unlikely, but still)
	 */
	u32 xchged_tail = xchg_tail(lock, *next_tail);

	if (likely(xchged_tail != _Q_LOCK_INVALID_TAIL)) {
		*old_tail = xchged_tail;
		return true;
	}

	/*
	 * If we got _Q_LOCK_INVALID_TAIL, it means lock was not in LOCK_MODE_QSPINLOCK.
	 * In this case we should restore _Q_LOCK_INVALID_TAIL
	 * and remember next contenders that got confused.
	 * Later we will update lock's or local queue's tail to the last contender seen here.
	 */
	u32 val = atomic_read(&lock->val);

	bool fixed = false;

	while (!fixed) {
		if (decode_lock_mode(val) == LOCK_MODE_QSPINLOCK) {
			*old_tail = 0;
			return true;
		}

		/*
		 * CAS is needed here to catch possible lock mode change
		 * from LOCK_MODE_HQLOCK to LOCK_MODE_QSPINLOCK in the meanwhile.
		 * Thus preventing from publishing _Q_LOCK_INVALID_TAIL
		 * when LOCK_MODE_QSPINLOCK is enabled.
		 */
		fixed = atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCK_INVALID_TAIL |
				(val & (_Q_LOCKED_PENDING_MASK | _Q_LOCK_TYPE_MODE_MASK)));
	}

	if ((val & _Q_TAIL_MASK) != tail)
		*next_tail = val & _Q_TAIL_MASK;

	return false;
}

/*
 * Call it if we observe LOCK_MODE_HQLOCK or LOCK_NO_MODE in the lock.
 *
 * Actions performed:
 * - Call setup_lock_mode to set or read lock's mode,
 *   read metadata's sequential counter for valid local queueing
 * - CAS on union of local tail and meta_seq_counter
 *   to guarantee metadata usage correctness.
 *   Repeat from the beginning if fail.
 * - If we are the first local contender,
 *   update global tail with our NUMA node
 */
static inline bool try_update_tail_hqlock_mode(struct qspinlock *lock, u16 lock_id,
				struct numa_qnode *qnode, u32 tail, u32 *next_tail, u32 *old_tail)
{
	u32 meta_seq_counter;
	hqlock_mode_t mode;

	struct numa_queue *queue;
	u64 old_counter_tail;
	bool updated_queue_tail = false;

re_setup:
	mode = setup_lock_mode(lock, lock_id, &meta_seq_counter);

	if (mode == LOCK_MODE_QSPINLOCK)
		return false;

	queue = get_local_queue(qnode);

	/*
	 * While queueing locally, perform CAS cycle
	 * on union of tail and meta_seq_counter.
	 *
	 * meta_seq_counter is taken from the lock metadata while allocation,
	 * it's updated every time it's used by a next lock.
	 * It shows that queue is used correctly
	 * and metadata hasn't been deoccupied before we queued locally.
	 */
	old_counter_tail = READ_ONCE(queue->seq_counter_tail);

	while (!updated_queue_tail &&
		   decode_tc_counter(old_counter_tail) == meta_seq_counter) {
		updated_queue_tail =
			try_cmpxchg_relaxed(&queue->seq_counter_tail, &old_counter_tail,
				encode_tc((*next_tail) >> _Q_TAIL_OFFSET, meta_seq_counter));
	}

	/* queue->seq_counter changed */
	if (!updated_queue_tail)
		goto re_setup;

	/*
	 * The condition means we tried to perform generic tail update in try_update_tail_qspinlock_mode,
	 * but before we did it, lock type was changed.
	 * Moreover, some contenders have come after us in LOCK_MODE_QSPINLOCK,
	 * during handoff we must notify them that they are set in LOCK_MODE_HQLOCK in our node's local queue
	 */
	if (unlikely(*next_tail != tail))
		qnode->wrong_fallback_tail = *next_tail >> _Q_TAIL_OFFSET;

	*old_tail = decode_tc_tail(old_counter_tail);

	if (!(*old_tail)) {
		u16 prev_node_id;

		init_queue(qnode);
		prev_node_id = append_node_queue(lock_id, qnode->numa_node);
		*old_tail = prev_node_id ? Q_NEW_NODE_QUEUE : 0;
	} else {
		*old_tail <<= _Q_TAIL_OFFSET;
	}

	return true;
}
