// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic numa-aware osq lock
 * Author: LiYong <yongli-oc@zhaoxin.com>
 *
 */
#include <linux/cpumask.h>
#include <asm/byteorder.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/osq_lock.h>
#include "numa.h"
#include "numa_osq.h"

int osq_node_max = 256;

/*
 * The pending bit spinning loop count.
 * This heuristic is used to limit the number of lockword accesses
 * made by atomic_cond_read_relaxed when waiting for the lock to
 * transition out of the "== _Q_PENDING_VAL" state. We don't spin
 * indefinitely because there's no guarantee that we'll make forward
 * progress.
 */

static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_cpu_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline int decode(int cpu_nr)
{
	return cpu_nr - 1;
}

static inline struct optimistic_spin_node *decode_curr(int encoded_cpu_val)
{
	int cpu_nr = decode(encoded_cpu_val);

	return per_cpu_ptr(&osq_cpu_node, cpu_nr);
}

static int atomic64_cmpxchg_notequal(void *qslock, atomic_t *tail, int curr)
{
	u64 ss = 0;
	u32 addr = ptrmask(qslock);
	u64 addrcurr = (((u64)addr) << 32) | curr;

	while (1) {
		ss = atomic64_read((atomic64_t *) tail);
		if ((ss >> 32) != addr)
			return NUMA_LOCKED_VAL;
		if ((ss & LOW32MASK) == NUMA_LOCKED_VAL)
			return NUMA_LOCKED_VAL;
		if (atomic64_cmpxchg((atomic64_t *) tail, ss, addrcurr) == ss)
			return ss & LOW32MASK;
		cpu_relax();
	}
}

void zx_osq_lock_stopping(struct optimistic_spin_queue *lock)
{
	int s = 0;

	s = zx_numa_lock_ptr_get(lock);
	if (s < zx_numa_lock_total) {
		numa_lock_init_data(zx_numa_entry[s].numa_ptr,
			NUMACLUSTERS, NUMA_UNLOCKED_VAL,
			ptrmask(lock));

		WRITE_ONCE(lock->index, s);
		zx_numa_entry[s].type = 1;
		smp_mb();/*should set these before enable*/
		prefetchw(&lock->numa_enable);
		WRITE_ONCE(lock->numa_enable, OSQLOCKSTOPPING);
	} else {
		prefetchw(&lock->numa_enable);
		WRITE_ONCE(lock->numa_enable, OSQLOCKINITED);
	}
}

void zx_osq_numa_start(struct optimistic_spin_queue *lock)
{
	struct _numa_lock *_numa_lock = get_numa_lock(lock->index);

	prefetchw(&lock->numa_enable);
	WRITE_ONCE(lock->numa_enable, NUMALOCKDYNAMIC);
	smp_mb(); /*should keep lock->numa_enable modified first*/
	atomic_set(&_numa_lock->initlock, TURNTONUMAREADY);
}


void zx_osq_turn_numa_waiting(struct optimistic_spin_queue *lock)
{
	struct _numa_lock *_numa_lock = get_numa_lock(lock->index);

	atomic_inc(&_numa_lock->pending);
	while (1) {
		int s = atomic_read(&_numa_lock->initlock);

		if (s == TURNTONUMAREADY)
			break;
		cpu_relax();
		cpu_relax();
		cpu_relax();
		cpu_relax();

	}
	atomic_dec(&_numa_lock->pending);
}




static struct optimistic_spin_node *
zx_numa_osq_wait_next(struct _numa_lock *lock,
		struct optimistic_spin_node *node,
		struct optimistic_spin_node *prev, int cpu)
{
	struct optimistic_spin_node *next = NULL;
	int curr = encode_cpu(cpu);
	int old;

	old = prev ? prev->cpu : OSQ_UNLOCKED_VAL;
	for (;;) {
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg_acquire(&lock->tail, curr, old) == curr) {

			break;
		}
		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next)
				break;
		}
		cpu_relax();
	}
	return next;
}
static void zx_numa_turn_osq_waiting(struct optimistic_spin_queue *lock,
			struct _numa_lock *_numa_lock)
{
	struct _numa_lock *numa_lock = _numa_lock + _numa_lock->numa_nodes;
	int lockaddr = ptrmask(lock);
	u64 s = 0;
	struct optimistic_spin_queue tail;

	tail.numa_enable = NUMALOCKDYNAMIC;
	tail.index = lock->index;
	tail.tail16 = OSQ_LOCKED_VAL;
	while (1) {
		cpu_relax(); cpu_relax(); cpu_relax(); cpu_relax();
		s = atomic64_read((atomic64_t *) &numa_lock->tail);
		if ((s >> 32) != lockaddr)
			break;
		if ((s & LOW32MASK) == NUMA_LOCKED_VAL)
			break;
	}
	prefetchw(&lock->tail);
	if (atomic_cmpxchg(&lock->tail, tail.val, OSQ_UNLOCKED_VAL)
			== tail.val) {
		;
	}

}

static int _zx_node_osq_lock_internal(struct optimistic_spin_queue *qslock,
	struct optimistic_spin_node *node, struct optimistic_spin_node *prev,
	struct _numa_lock *node_lock, int cpu, int *cur_status)
{
	struct optimistic_spin_node *next = NULL;

	for (;;) {
		struct optimistic_spin_node *node_prev = NULL;

		if (prev->next == node &&
		    cmpxchg(&prev->next, node, NULL) == node) {
			break;
		}
		/*load locked first each time*/
		*cur_status = smp_load_acquire(&node->locked);

		if (*cur_status != NODE_WAIT)
			return 0; //goto NODE_UNLOCK;

		cpu_relax();
		node_prev = READ_ONCE(node->prev);
		if (node_prev != prev)
			prev = node_prev;
	}

	next = zx_numa_osq_wait_next(node_lock, node, prev, cpu);
	if (!next)
		return -1;

	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);

	return -1;
}

static int _zx_node_osq_lock(struct optimistic_spin_queue *qslock,
					struct _numa_lock *_numa_lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_cpu_node);
	struct optimistic_spin_node *prev = NULL;
	int cpu = smp_processor_id();
	int curr = encode_cpu(cpu);
	int numa = cpu >> _numa_lock->shift;
	struct _numa_lock *node_lock = _numa_lock + numa;
	int cur_status = 0;
	int old = 0;

	node->locked = NODE_WAIT;
	node->next = NULL;
	node->cpu = curr;

	old = atomic64_cmpxchg_notequal(qslock, &node_lock->tail, curr);

	if (old == NUMA_LOCKED_VAL) {
		bool s = true;

		zx_numa_turn_osq_waiting(qslock, _numa_lock);
		s = osq_lock(qslock);
		if (s == true)
			return 1;
		else
			return -1;
	}

	if (old == 0) {
		node->locked = COHORT_START;
		return ACQUIRE_NUMALOCK;
	}

	prev = decode_curr(old);
	node->prev = prev;

	smp_mb(); /* make sure node set before set pre->next */

	WRITE_ONCE(prev->next, node);

	while ((cur_status = READ_ONCE(node->locked)) == NODE_WAIT) {
		if (need_resched() || vcpu_is_preempted(node_cpu(node->prev))) {
			int ddd = _zx_node_osq_lock_internal(qslock, node, prev,
						node_lock, cpu, &cur_status);

			if (cur_status != NODE_WAIT)
				goto NODE_UNLOCK;
			if (ddd == -1)
				return -1;
		}
		cpu_relax();
	}
NODE_UNLOCK:
	if (cur_status == ACQUIRE_NUMALOCK)
		node->locked = COHORT_START;
	return cur_status;
}
static int _zx_numa_osq_lock(struct optimistic_spin_queue *qslock, int cpu,
				struct _numa_lock *_numa_lock)
{
	int numacpu = cpu >> _numa_lock->shift;
	int numacurr = encode_cpu(numacpu);

	struct optimistic_spin_node *node = &(_numa_lock + numacpu)->osq_node;
	struct _numa_lock *numa_lock = _numa_lock + _numa_lock->numa_nodes;
	struct optimistic_spin_node *prevnode = NULL;
	int prev = 0;

	node->next = NULL;
	node->locked = LOCK_NUMALOCK;
	node->cpu = numacurr;

	prev = atomic_xchg(&numa_lock->tail, numacurr);
	if (prev == 0) {
		node->locked = UNLOCK_NUMALOCK;
		return 0;
	}

	prevnode = &(_numa_lock + prev - 1)->osq_node;
	node->prev = prevnode;
	smp_mb(); /*node->prev should be set before next*/
	WRITE_ONCE(prevnode->next, node);

	while (READ_ONCE(node->locked) == LOCK_NUMALOCK) {
		cpu_relax();
		cpu_relax();
		cpu_relax();
		cpu_relax();
	}
	return 0;
}
inline bool zx_numa_osq_lock(struct optimistic_spin_queue *qslock,
		struct _numa_lock *_numa_lock)
{
	struct _numa_lock *node_lock = NULL;
	int cpu = smp_processor_id();
	int numa = cpu >> _numa_lock->shift;
	int status = 0;

	node_lock = _numa_lock + numa;

	if (node_lock->stopping) {
		zx_numa_turn_osq_waiting(qslock, _numa_lock);
		return osq_lock(qslock);
	}

	status = _zx_node_osq_lock(qslock, _numa_lock);
	if (status == ACQUIRE_NUMALOCK)
		status = _zx_numa_osq_lock(qslock, smp_processor_id(),
				_numa_lock);

	if (status == -1)
		return false;
	return true;
}

static int atomic64_checktail_osq(struct optimistic_spin_queue *qslock,
	struct _numa_lock *node_lock, int ctail)
{
	u64 addr = ((u64)ptrmask(qslock)) << 32;
	u64 addrtail = addr | ctail;
	u64 ss = 0;
	bool mark;

	ss = atomic64_read((atomic64_t *) &node_lock->tail);
	if (node_lock->stopping == 0)
		mark = (ss == addrtail &&
			atomic64_cmpxchg_acquire(
				(atomic64_t *) &node_lock->tail,
				addrtail, addr|NUMA_UNLOCKED_VAL) == addrtail);
	else
		mark = (ss == addrtail &&
			atomic64_cmpxchg_acquire(
				(atomic64_t *) &node_lock->tail,
				addrtail, NUMA_LOCKED_VAL) == addrtail);
	return mark;
}

static void node_lock_release(struct optimistic_spin_queue *qslock,
		struct _numa_lock *node_lock, struct optimistic_spin_node *node,
		int val, int cpu, int numa_end)
{
	struct optimistic_spin_node *next = NULL;
	int curr = encode_cpu(cpu);

	while (1) {
		if (atomic64_checktail_osq(qslock, node_lock, curr)) {
			if (qslock->numa_enable == NUMALOCKDYNAMIC) {
				int index = qslock->index;

				if (numa_end == OSQ_UNLOCKED_VAL &&
					zx_numa_entry[index].idle == 0) {
					cmpxchg(&zx_numa_entry[index].idle,
							0, 1);
				}
			}
			return;
		}
		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next) {
				WRITE_ONCE(next->locked, val);
				return;
			}
		}
		cpu_relax();
	}
}

static int numa_lock_release(struct optimistic_spin_queue *qslock,
		struct _numa_lock *numa_lock,
		struct optimistic_spin_node *node, int cpu)
{
	struct optimistic_spin_node *next = NULL;
	int curr = cpu >> numa_lock->shift;
	int numacurr = encode_cpu(curr);

	while (1) {
		if (atomic_read(&numa_lock->tail) == numacurr &&
		    atomic_cmpxchg_acquire(&numa_lock->tail, numacurr,
					   OSQ_UNLOCKED_VAL) == numacurr) {
			return OSQ_UNLOCKED_VAL;
		}

		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next) {
				WRITE_ONCE(next->locked, UNLOCK_NUMALOCK);
				return 1;
			}
		}
		cpu_relax();
	}
}

inline void zx_numa_osq_unlock(struct optimistic_spin_queue *qslock,
		 struct _numa_lock *_numa_lock)
{
	u32 cpu =  smp_processor_id();
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_cpu_node);
	int numa = cpu >> _numa_lock->shift;
	struct _numa_lock *numa_lock = _numa_lock + _numa_lock->numa_nodes;
	struct _numa_lock *node_lock = _numa_lock + numa;
	struct optimistic_spin_node *numa_node =
						&(_numa_lock + numa)->osq_node;
	struct optimistic_spin_node *next = NULL;
	int cur_count = 0;
	int numa_end = 0;

	cur_count = READ_ONCE(node->locked);

	if (cur_count >= osq_node_max - 1) {
		numa_end = numa_lock_release(qslock,
				numa_lock, numa_node, cpu);
		node_lock_release(qslock, node_lock, node,
				ACQUIRE_NUMALOCK, cpu, numa_end);
		return;
	}

	next = xchg(&node->next, NULL);
	if (next) {
		WRITE_ONCE(next->locked, cur_count + 1);
		return;
	}

	numa_end = numa_lock_release(qslock, numa_lock, numa_node, cpu);
	node_lock_release(qslock, node_lock, node, ACQUIRE_NUMALOCK,
			cpu, numa_end);
}
