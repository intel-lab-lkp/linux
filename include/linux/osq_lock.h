/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_OSQ_LOCK_H
#define __LINUX_OSQ_LOCK_H

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 */

struct optimistic_spin_queue {
	/*
	 * Stores an encoded value of the CPU # of the tail node in the queue.
	 * If the queue is empty, then it's set to OSQ_UNLOCKED_VAL.
	 */
#ifdef CONFIG_LOCK_SPIN_ON_OWNER_NUMA
	union {
		atomic_t tail;
		u32 val;
#ifdef __LITTLE_ENDIAN
		struct {
			u16 tail16;
			u8 index;
			u8 numa_enable;
		};
#else
		struct {
			u8 numa_enable;
			u8 index;
			u16 tail16;
		};
#endif
	};
#else
	atomic_t tail;
#endif
};

#define OSQ_UNLOCKED_VAL (0)

/* Init macro and function. */
#ifdef CONFIG_LOCK_SPIN_ON_OWNER_NUMA

#define OSQ_LOCK_UNLOCKED { .tail = ATOMIC_INIT(OSQ_UNLOCKED_VAL) }

#else

#define OSQ_LOCK_UNLOCKED { ATOMIC_INIT(OSQ_UNLOCKED_VAL) }

#endif

static inline void osq_lock_init(struct optimistic_spin_queue *lock)
{
	atomic_set(&lock->tail, OSQ_UNLOCKED_VAL);
}

extern bool osq_lock(struct optimistic_spin_queue *lock);
extern void osq_unlock(struct optimistic_spin_queue *lock);

#ifdef CONFIG_LOCK_SPIN_ON_OWNER_NUMA
extern bool osq_is_locked(struct optimistic_spin_queue *lock);
#else
static inline bool osq_is_locked(struct optimistic_spin_queue *lock)
{
	return atomic_read(&lock->tail) != OSQ_UNLOCKED_VAL;
}
#endif
#endif
