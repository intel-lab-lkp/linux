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
 * The 'next' pointer of the tail must be zero, all the other 'next' pointers
 * must either be valid or transiently zero.
 * The 'prev' pointer is zero unless the node is waiting for the lock, when
 * waiting it may refer to the wrong node (node->prev->next != node).
 * The 'prev' value is only needed for the node->prev->next = node->next update
 * when 'node' is being removed. Atomically checking node->prev->next == node
 * ensures the list doesn't get corrupted.
 */

struct optimistic_spin_node {
	unsigned int next; /* CPU number offset by 1, 0 if no next */
	unsigned int prev; /* CPU number offset by 1, 0 if lock held */
} __aligned(8);

static DEFINE_PER_CPU(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline unsigned int encode_cpu(unsigned int cpu_nr)
{
	return cpu_nr + 1;
}

static inline struct optimistic_spin_node *
decode_cpu(unsigned int encoded_cpu_val)
{
	return per_cpu_ptr(&osq_node, encoded_cpu_val - 1);
}

/*
 * Unlink the current cpu's node from the lock's node->prev list.
 *
 * More specifically atomically write its node->prev over the link that
 * currently points to node.
 * This is either:
 *    lock->tail = node->prev
 * or:
 *    node->next->prev = node->prev
 * The first is a simple cmpxchg(), the second is protected against
 * node->next trying to unlink itself (after need_resched() is set) by using
 * an xchg() on node->next that sets it to NULL.
 *
 * When a lock request is being cancelled the caller needs 'next' to
 * set node->prev->next = next.
 */
static inline unsigned int
osq_unlink_from_next(struct optimistic_spin_queue *lock, unsigned int prev)
{
	unsigned int curr = encode_cpu(smp_processor_id());
	struct optimistic_spin_node *node;
	unsigned int next;

	for (;;) {
		unsigned int tail = READ_ONCE(lock->tail);
		if (curr == tail &&
		    try_cmpxchg_release(&lock->tail, &tail, prev)) {
			/*
			 * We were the last queued, lock->tail now references
			 * prev (or is 0 if the list is now empty).
			 * If prev was spinning in this loop it can continue.
			 *
			 * Since we are the tail of the list, node->next
			 * must be zero.
			 */
			return 0;
		}

		node = this_cpu_ptr(&osq_node);

		/*
		 * We must xchg() the @node->next value to ensure that a
		 * concurrent unqueue() from @node->next will find an invalid
		 * @prev value (node_next->prev->next != node_next).
		 *
		 * If @node->next is already NULL then we need to wait until
		 * the concurrent unqueue completes.
		 */
		if (node->next) {
			next = xchg(&node->next, 0);
			if (next)
				break;
		}

		cpu_relax();
	}

	/*
	 * When called from osq_unlock() prev is zero and this hands
	 * over the lock ownership.
	 * When called while unqueueing in osq_lock() this completes the
	 * backwards link, the forwards link is done by the caller.
	 */
	WRITE_ONCE(decode_cpu(next)->prev, prev);

	return next;
}

bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *prev_ptr;
	unsigned int curr = encode_cpu(smp_processor_id());
	unsigned int next, prev;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */
	prev = xchg(&lock->tail, curr);
	if (prev == OSQ_UNLOCKED_VAL)
		return true;

	node = this_cpu_ptr(&osq_node);
	prev_ptr = decode_cpu(prev);
	node->prev = prev;

	/*
	 * osq_lock()			unqueue
	 *
	 * node->prev = prev		osq_unlink_from_next()
	 * WMB				MB
	 * prev->next = node		next->prev = prev // unqueue-C
	 *
	 * Here 'node->prev' and 'next->prev' are the same variable and we need
	 * to ensure these stores happen in-order to avoid corrupting the list.
	 */
	smp_wmb();

	WRITE_ONCE(prev_ptr->next, curr);

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

		if (data_race(prev_ptr->next) == curr &&
		    cmpxchg(&prev_ptr->next, curr, 0) == curr)
			break;

		/*
		 * 'prev' must have unlinked (or be in the process of unlinking)
		 * itself from the list.
		 */

		cpu_relax();
	}

	/*
	 * If 'prev' tries to remove itself from the list before we write
	 * a new value to prev->next it will spin in osq_unlink_from_next().
	 * This means we can no longer be given the lock and always
	 * return false.
	 */

	/*
	 * Invalidate prev matching osq_unlock().
	 * This isn't necessary but ensures that both unlocked and fast-path
	 * locked nodes (where the initial xchg() returned 0) have prev set
	 * to zero.
	 * If nothing else it lets the lock chain be followed from lock->tail
	 * whch may help diagnostics.
	 */
	node->prev = 0;

	/*
	 * Now that the linkage to prev cannot change underneath us
	 * remove ourselves from the node->prev list.
	 * This does:
	 * (node->next ? node->next->prev : lock->tail) = node->prev
	 */
	next = osq_unlink_from_next(lock, prev);

	/*
	 * Finally mend the node->next list that was 'broken' to
	 * stop node->prev trying to unlink from us.
	 * If next is NULL then lock->tail is prev_ptr and another node
	 * can be added - so we must not re-write the NULL.
	 */
	if (next) {
		/*
		 * This must happen after the write to node->next->prev.
		 * If swapped then prev could unlink itself before our
		 * write to node->next->prev and the the wrong value would
		 * end up in node->next->prev.
		 * Probably can't actually happen due to re-ordering of writes,
		 * but could happen without a compiler barrier.
		 */
		smp_wmb();
		WRITE_ONCE(prev_ptr->next, next);
	}

	return false;
}

void osq_unlock(struct optimistic_spin_queue *lock)
{
	osq_unlink_from_next(lock, OSQ_UNLOCKED_VAL);
}
