// SPDX-License-Identifier: GPL-2.0
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>

/*
 * An MCS like spin lock especially tailored for optimistic spinning for
 * sleeping lock implementations (mutex, rwsem, etc).
 * Each CPU spins on a local variable to avoid cache-line bounces.
 *
 * The CPU that holds the osq_lock checks the mutex/rwsem, the other CPU spin
 * in osq_lock() until either the osq_lock is obtained or the scheduler
 * requests the process be preempted.
 *
 * Using a single osq node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 *
 * The osq_nodes for the spinning CPU are put on a double-linked (non circular)
 * list. The list 'pointers' can either be the address of the osq_node or the
 * associated CPU number, the CPU numbers are offset by one so that zero can
 * be used like a NULL ponter.
 * The mutex/rwsem contains a pointer (CPU number) to the tail of the list.
 * There is no equivalent pointer to the list head - the 'head' is the
 * osq_node of the CPU that acquired the osq lock.
 *
 * The 'next' pointer of the tail must be NULL, all the other 'next' pointers
 * must either be valid or transiently NULL.
 * The 'prev' pointers only need to be valid when node->prev makes sense and,
 * even then, can be transiently invalid (ie refer to the wrong node).
 * They are only used for the node->prev->next = node->next update when
 * 'node' is being removed. Atomically checking node->prev->next == node
 * ensures the list doesn't get corrupted.
 */

struct optimistic_spin_node {
	struct optimistic_spin_node *next;
	int prev; /* CPU number offset by 1 */
};

static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline int encode_cpu(int cpu_nr)
{
	return cpu_nr + 1;
}

static inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val)
{
	int cpu_nr = encoded_cpu_val - 1;

	return per_cpu_ptr(&osq_node, cpu_nr);
}

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
osq_wait_next(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      int old_cpu)
{
	int curr = encode_cpu(smp_processor_id());

	for (;;) {
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg_acquire(&lock->tail, curr, old_cpu) == curr) {
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

bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	struct optimistic_spin_node *prev_ptr, *next;
	int curr = encode_cpu(smp_processor_id());
	int prev;

	node->next = NULL;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */
	prev = atomic_xchg(&lock->tail, curr);
	if (prev == OSQ_UNLOCKED_VAL)
		return true;

	prev_ptr = decode_cpu(prev);
	node->prev = prev;

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

	WRITE_ONCE(prev_ptr->next, node);

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
	prev = smp_cond_load_relaxed(&node->prev, !VAL || need_resched() ||
				     vcpu_is_preempted(VAL - 1));

	/*
	 * Step - A
	 *
	 * Loop until either node->prev is zero (lock acquired) or we
	 * atomically change prev->next from node to NULL (stopping prev
	 * handing on the lock).
	 * Note that 'prev' can unlink itself concurrently with this
	 * test so that prev/prev_ptr can be stale, but since it
	 * is per-cpu data the memory can always be read.
	 */

	for (;; prev = READ_ONCE(node->prev)) {
		if (!prev)
			/* Lock acquired */
			return true;

		prev_ptr = decode_cpu(prev);

		if (data_race(prev_ptr->next) == node &&
		    cmpxchg(&prev_ptr->next, node, NULL) == node)
			break;

		/*
		 * 'prev' must have unlinked (or be in the process of unlinking)
		 * itself from the list.
		 */

		cpu_relax();
	}

	/*
	 * If 'prev' tries to remove itself from the list before we write
	 * a new value to prev->next it will spin in osq_wait_next().
	 */

	/* Invalidate prev_cpu matching osq_unlock() */
	node->prev = 0;

	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */

	next = osq_wait_next(lock, node, prev);
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
	WRITE_ONCE(prev_ptr->next, next);

	return false;
}

void osq_unlock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next;
	int curr = encode_cpu(smp_processor_id());

	/*
	 * Fast path for the uncontended case.
	 */
	if (atomic_try_cmpxchg_release(&lock->tail, &curr, OSQ_UNLOCKED_VAL))
		return;

	/*
	 * Second most likely case.
	 */
	node = this_cpu_ptr(&osq_node);
	next = xchg(&node->next, NULL);
	if (next) {
		WRITE_ONCE(next->prev, 0);
		return;
	}

	next = osq_wait_next(lock, node, OSQ_UNLOCKED_VAL);
	if (next)
		WRITE_ONCE(next->prev, 0);
}
