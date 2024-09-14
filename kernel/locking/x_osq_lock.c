// SPDX-License-Identifier: GPL-2.0
/*
 * crossing from osq_lock to numa-aware lock
 */
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>
#include "numa.h"
#include "numa_osq.h"

u16 osq_lock_depth = 8;
u16 osq_keep_times = 32;

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 *
 * Using a single mcs node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 */
DECLARE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * Get a stable @node->next pointer, either for unlock() or unqueue() purposes.
 * Can return NULL in case we were the last queued and we updated @lock instead.
 *
 * If osq_lock() is being cancelled there must be a previous node
 * and 'old_cpu' is its CPU #.
 * For osq_unlock() there is never a previous node and old_cpu is
 * set to OSQ_UNLOCKED_VAL.
 */
static inline struct optimistic_spin_node *
osq_wait_next_stop(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      int old_cpu)
{
	u16 curr = encode_cpu(smp_processor_id());
	u16 old = old_cpu;

	if (lock->numa_enable == OSQLOCKSTOPPING && old == OSQ_UNLOCKED_VAL)
		old = OSQ_LOCKED_VAL;

	for (;;) {
		if (READ_ONCE(lock->tail16) == curr &&
		    cmpxchg(&lock->tail16, curr, old) == curr) {

			/*
			 * We were the last queued, we moved @lock back. @prev
			 * will now observe @lock and will complete its
			 * unlock()/unqueue().
			 */
			return NULL;
		}

		/*
		 * We must xchg() the @node->next value, because if we were to
		 * leave it in, a concurrent unlock()/unqueue() from
		 * @node->next might complete Step-A and think its @prev is
		 * still valid.
		 *
		 * If the concurrent unlock()/unqueue() wins the race, we'll
		 * wait for either @lock to point to us, through its Step-B, or
		 * wait for a new @node->next from its Step-C.
		 */
		if (node->next) {
			struct optimistic_spin_node *next;

			next = xchg(&node->next, NULL);
			if (next)
				return next;
		}

		cpu_relax();
	}
}

bool x_osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	struct optimistic_spin_node *prev, *next;
	int cpu = smp_processor_id();
	u16 curr = encode_cpu(cpu);
	struct optimistic_spin_queue tail;
	u16 old;

	tail.val = READ_ONCE(lock->val);
	if (unlikely(tail.numa_enable == OSQLOCKSTOPPING)) {
		zx_osq_turn_numa_waiting(lock);
		return x_osq_lock(lock);
	}

	if (unlikely(tail.numa_enable == NUMALOCKDYNAMIC)) {
		struct _numa_lock *_numa_lock = NULL;
		struct _numa_lock *node_lock = NULL;

		_numa_lock = get_numa_lock(tail.index);
		node_lock = (struct _numa_lock *) _numa_lock +
					(cpu >> NUMASHIFT);

		prefetch(node_lock);
		return zx_numa_osq_lock(lock, _numa_lock);
	}

	node->locked = 0;
	node->next = NULL;
	node->cpu = curr;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */

	if (likely(tail.numa_enable >= OSQTONUMADETECT)) {
		struct optimistic_spin_queue ss;

		while (1) {
			ss.val = atomic_read(&lock->tail);
			if (ss.tail16 == OSQ_LOCKED_VAL) {
				zx_osq_turn_numa_waiting(lock);
				return x_osq_lock(lock);
			}
			if (cmpxchg(&lock->tail16, ss.tail16, curr)
					== ss.tail16) {
				old = ss.tail16;
				break;
			}
			cpu_relax();
		}
	} else
		old = xchg(&lock->tail16, curr);

	if (old == OSQ_UNLOCKED_VAL) {
		node->serial = 1;
		return true;
	}

	prev = decode_cpu(old);
	node->prev = prev;

	node->serial = prev->serial + 1;
	/*
	 * osq_lock()			unqueue
	 *
	 * node->prev = prev		osq_wait_next()
	 * WMB				MB
	 * prev->next = node		next->prev = prev // unqueue-C
	 *
	 * Here 'node->prev' and 'next->prev' are the same variable and we need
	 * to ensure these stores happen in-order to avoid corrupting the list.
	 */
	smp_wmb();

	WRITE_ONCE(prev->next, node);

	/*
	 * Normally @prev is untouchable after the above store; because at that
	 * moment unlock can proceed and wipe the node element from stack.
	 *
	 * However, since our nodes are static per-cpu storage, we're
	 * guaranteed their existence -- this allows us to apply
	 * cmpxchg in an attempt to undo our queueing.
	 */

	/*
	 * Wait to acquire the lock or cancellation. Note that need_resched()
	 * will come with an IPI, which will wake smp_cond_load_relaxed() if it
	 * is implemented with a monitor-wait. vcpu_is_preempted() relies on
	 * polling, be careful.
	 */
	if (smp_cond_load_relaxed(&node->locked, VAL || need_resched() ||
				  vcpu_is_preempted(node_cpu(node->prev))))
		return true;

	/* unqueue */
	/*
	 * Step - A  -- stabilize @prev
	 *
	 * Undo our @prev->next assignment; this will make @prev's
	 * unlock()/unqueue() wait for a next pointer since @lock points to us
	 * (or later).
	 */

	for (;;) {
		/*
		 * cpu_relax() below implies a compiler barrier which would
		 * prevent this comparison being optimized away.
		 */
		if (data_race(prev->next) == node &&
		    cmpxchg(&prev->next, node, NULL) == node)
			break;

		/*
		 * We can only fail the cmpxchg() racing against an unlock(),
		 * in which case we should observe @node->locked becoming
		 * true.
		 */
		if (smp_load_acquire(&node->locked))
			return true;

		cpu_relax();

		/*
		 * Or we race against a concurrent unqueue()'s step-B, in which
		 * case its step-C will write us a new @node->prev pointer.
		 */
		prev = READ_ONCE(node->prev);
	}

	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */

	next = osq_wait_next_stop(lock, node, prev->cpu);
	if (!next)
		return false;

	/*
	 * Step - C -- unlink
	 *
	 * @prev is stable because its still waiting for a new @prev->next
	 * pointer, @next is stable because our @node->next pointer is NULL and
	 * it will wait in Step-A.
	 */

	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);

	return false;
}



void x_osq_unlock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next;
	int threadshold = osq_lock_depth;
	int cpu = smp_processor_id();
	u16 curr = encode_cpu(cpu);
	int depth = 0;
	u32 count = 0;

	if (unlikely(lock->numa_enable == NUMALOCKDYNAMIC)) {
		struct _numa_lock *_numa_lock = get_numa_lock(lock->index);

		prefetch((struct _numa_lock *) _numa_lock + (cpu >> NUMASHIFT));
		return zx_numa_osq_unlock(lock, _numa_lock);
	}
	/*
	 * Fast path for the uncontended case.
	 */
	if (unlikely(lock->numa_enable == OSQTONUMADETECT)) {
		struct optimistic_spin_node *node_last = NULL;
		u16 tail = 0;

		tail = cmpxchg(&lock->tail16, curr, OSQ_UNLOCKED_VAL);
		if (tail == curr)
			return;

		node = this_cpu_ptr(&osq_node);
		node_last = decode_cpu(tail);
		depth = node_last->serial - node->serial;
		count = READ_ONCE(node->locked);
		if (count > osq_keep_times && (dynamic_enable & 0x1))
			zx_osq_lock_stopping(lock);
	} else if (unlikely(lock->numa_enable == OSQLOCKSTOPPING)) {
		if (cmpxchg(&lock->tail16, curr, OSQ_LOCKED_VAL)
					== curr) {
			zx_osq_numa_start(lock);
			return;
		}
	} else {
		struct optimistic_spin_queue t;

		t.val = 0;
		if (dynamic_enable & 0x1) {
			if (atomic_read(&numa_count) < zx_numa_lock_total)
				t.numa_enable = OSQTONUMADETECT;
		}
		if (t.numa_enable == OSQTONUMADETECT) {
			if (atomic_cmpxchg_release(&lock->tail, curr,
				(t.val | OSQ_UNLOCKED_VAL)) == curr)
				return;
		} else if (cmpxchg(&lock->tail16, curr,
				OSQ_UNLOCKED_VAL) == curr)
			return;
	}

	/*
	 * Second most likely case.
	 */
	node = this_cpu_ptr(&osq_node);
	next = xchg(&node->next, NULL);
	if (next) {
		if (depth > threadshold)
			WRITE_ONCE(next->locked, count + 1);
		else
			WRITE_ONCE(next->locked, 1);
		return;
	}

	next = osq_wait_next_stop(lock, node, OSQ_UNLOCKED_VAL);
	if (next) {
		if (depth > threadshold)
			WRITE_ONCE(next->locked, count + 1);
		else
			WRITE_ONCE(next->locked, 1);
	}
}

bool x_osq_is_locked(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_queue val;

	val.val = atomic_read(&lock->tail);
	if (val.tail16 == OSQ_UNLOCKED_VAL)
		return false;

	if (val.tail16 == OSQ_LOCKED_VAL) {
		if (val.numa_enable != NUMALOCKDYNAMIC)
			return true;
		return zx_check_numa_dynamic_locked(ptrmask(lock),
					get_numa_lock(val.index), 0);
	}

	return true;
}
