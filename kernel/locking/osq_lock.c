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
 * list similar to an hlist.
 * The list 'pointers' are the CPU numbers (offset by one so that zero can
 * be used like a NULL ponter).
 * Waiting cpu are added to the head of the list, the tail of the list is
 * the osq_node of the CPU that acquired the osq lock.
 *
 * The 'prev' pointer of the head must be zero, all the other 'prev' pointers
 * must either be valid or transiently zero.
 * The 'next' pointer is zero unless the node is waiting for the lock, when
 * waiting it may refer to the wrong node (node->next->prev != node).
 * The 'next' value is only needed for the node->next->prev = node->prev update
 * when 'node' is being removed. Atomically checking node->next->prev == node
 * ensures the list doesn't get corrupted.
 */

struct optimistic_spin_node {
	unsigned int next; /* CPU number offset by 1, 0 if lock held */
	unsigned int prev; /* CPU number offset by 1, 0 if no prev */
} __aligned(8);

static DEFINE_PER_CPU(struct optimistic_spin_node, osq_node);

static inline struct optimistic_spin_node *
cpu_spin_node(unsigned int offset_cpu_num)
{
	return per_cpu_ptr(&osq_node, offset_cpu_num - 1);
}

/*
 * Unlink the current cpu's node from the lock's node->next list.
 *
 * More specifically atomically write its node->next over the link that
 * currently points to node.
 * This is either:
 *    lock->head = node->next
 * or:
 *    node->prev->next = node->next
 * The first is a simple cmpxchg(), the second is protected against
 * node->prev trying to unlink itself (after need_resched() is set) by using
 * an xchg() on node->prev that sets it to NULL.
 *
 * When a lock request is being cancelled the caller needs 'prev' to
 * set node->next->prev = prev.
 */
static inline unsigned int
osq_unlink_from_prev(struct optimistic_spin_queue *lock, unsigned int next)
{
	unsigned int curr = smp_processor_id() + 1;
	struct optimistic_spin_node *node;
	unsigned int prev;

	for (;;) {
		unsigned int head = READ_ONCE(lock->head);
		if (curr == head &&
		    try_cmpxchg_release(&lock->head, &head, next)) {
			/*
			 * We were the last queued, lock->head now references
			 * next (or is 0 if the list is now empty).
			 * If next was spinning in this loop it can continue.
			 *
			 * Since we are the head of the list, node->prev
			 * must be zero.
			 */
			return 0;
		}

		node = this_cpu_ptr(&osq_node);

		/*
		 * We must xchg() the @node->prev value to ensure that a
		 * concurrent unqueue() from @node->prev will find an invalid
		 * @next value (node_prev->next->prev != node_prev).
		 *
		 * If @node->prev is already NULL then we need to wait until
		 * the concurrent unqueue completes.
		 */
		if (node->prev) {
			prev = xchg(&node->prev, 0);
			if (prev)
				break;
		}

		cpu_relax();
	}

	/*
	 * When called from osq_unlock() next is zero and this hands
	 * over the lock ownership.
	 * When called while unqueueing in osq_lock() this completes the
	 * backwards link, the forwards link is done by the caller.
	 */
	WRITE_ONCE(cpu_spin_node(prev)->next, next);

	return prev;
}

bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next_ptr;
	unsigned int curr = smp_processor_id() + 1;
	unsigned int prev, next;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock head.
	 */
	next = xchg(&lock->head, curr);
	if (next == OSQ_UNLOCKED_VAL)
		return true;

	node = this_cpu_ptr(&osq_node);
	next_ptr = cpu_spin_node(next);
	node->next = next;

	/*
	 * osq_lock()			unqueue
	 *
	 * node->next = next		osq_unlink_from_prev()
	 * WMB				MB
	 * next->prev = node		prev->next = next // unqueue-C
	 *
	 * Here 'node->next' and 'prev->next' are the same variable and we need
	 * to ensure these stores happen in-order to avoid corrupting the list.
	 */
	smp_wmb();

	WRITE_ONCE(next_ptr->prev, curr);

	/*
	 * Normally @next is untouchable after the above store; because at that
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
	next = smp_cond_load_relaxed(&node->next, !VAL || need_resched() ||
				     vcpu_is_preempted(VAL - 1));

	/*
	 * Loop until either node->next is zero (lock acquired) or we
	 * atomically change next->prev from node to NULL (stopping next
	 * handing on the lock).
	 * Note that 'next' can unlink itself concurrently with this
	 * test so that next/next_ptr can be stale, but since it
	 * is per-cpu data the memory can always be read.
	 */

	for (;; next = READ_ONCE(node->next)) {
		if (!next)
			/* Lock acquired */
			return true;

		next_ptr = cpu_spin_node(next);

		if (data_race(next_ptr->prev) == curr &&
		    cmpxchg(&next_ptr->prev, curr, 0) == curr)
			break;

		/*
		 * 'next' must have unlinked (or be in the process of unlinking)
		 * itself from the list.
		 */

		cpu_relax();
	}

	/*
	 * If 'next' tries to remove itself from the list before we write
	 * a new value to next->prev it will spin in osq_unlink_from_prev().
	 * This means we can no longer be given the lock and always
	 * return false.
	 */

	/*
	 * Invalidate next matching osq_unlock().
	 * This isn't necessary but ensures that both unlocked and fast-path
	 * locked nodes (where the initial xchg() returned 0) have next set
	 * to zero.
	 * If nothing else it lets the lock chain be followed from lock->head
	 * whch may help diagnostics.
	 */
	node->next = 0;

	/*
	 * Now that the linkage to next cannot change underneath us
	 * remove ourselves from the node->next list.
	 * This does:
	 * (node->prev ? node->prev->next : lock->head) = node->next
	 */
	prev = osq_unlink_from_prev(lock, next);

	/*
	 * Finally mend the node->prev list that was 'broken' to
	 * stop node->next trying to unlink from us.
	 * If prev is NULL then lock->head is next_ptr and another node
	 * can be added - so we must not re-write the NULL.
	 */
	if (prev) {
		/*
		 * This must happen after the write to node->prev->next.
		 * If swapped then next could unlink itself before our
		 * write to node->prev->next and the the wrong value would
		 * end up in node->prev->next.
		 * Probably can't actually happen due to re-ordering of writes,
		 * but could happen without a compiler barrier.
		 */
		smp_wmb();
		WRITE_ONCE(next_ptr->prev, prev);
	}

	return false;
}

void osq_unlock(struct optimistic_spin_queue *lock)
{
	osq_unlink_from_prev(lock, OSQ_UNLOCKED_VAL);
}
