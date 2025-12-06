/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Queued spinlock
 *
 * (C) Copyright 2013-2015 Hewlett-Packard Development Company, L.P.
 *
 * Authors: Waiman Long <waiman.long@hp.com>
 */
#ifndef __ASM_GENERIC_QSPINLOCK_TYPES_H
#define __ASM_GENERIC_QSPINLOCK_TYPES_H

#include <linux/types.h>

typedef struct qspinlock {
	union {
		atomic_t val;

		/*
		 * By using the whole 2nd least significant byte for the
		 * pending bit, we can allow better optimization of the lock
		 * acquisition for the pending bit holder.
		 */
#ifdef __LITTLE_ENDIAN
		struct {
			u8	locked;
			u8	pending;
		};
		struct {
			u16	locked_pending;
#ifndef CONFIG_HQSPINLOCKS
			u16	tail;
#else
		union {
			u16 tail;
			struct {
				u8	tail_node;
				u8	head_node;
			};
		};
#endif
		};
#else
		struct {
			u16	tail;
			u16	locked_pending;
		};
		struct {
			u8	reserved[2];
			u8	pending;
			u8	locked;
		};
#endif
	};
} arch_spinlock_t;

/*
 * Bitfields in the atomic value:
 *
 * When NR_CPUS < 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-15: not used
 * 16-17: tail index
 * 18-31: tail cpu (+1)
 *
 * When NR_CPUS >= 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-10: tail index
 * 11-31: tail cpu (+1)
 */
#define	_Q_SET_MASK(type)	(((1U << _Q_ ## type ## _BITS) - 1)\
				      << _Q_ ## type ## _OFFSET)
#define _Q_LOCKED_OFFSET	0
#define _Q_LOCKED_BITS		8
#define _Q_LOCKED_MASK		_Q_SET_MASK(LOCKED)

#define _Q_PENDING_OFFSET	(_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#if CONFIG_NR_CPUS < (1U << 14)
#define _Q_PENDING_BITS		8
#else
#define _Q_PENDING_BITS		1
#endif

#ifdef CONFIG_HQSPINLOCKS
/* For locks with HQ-mode we always use single pending bit */
#define _Q_PENDING_HQLOCK_BITS	 1
#define _Q_PENDING_HQLOCK_OFFSET (_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_PENDING_HQLOCK_MASK		_Q_SET_MASK(PENDING_HQLOCK)

#define _Q_LOCKTYPE_OFFSET	(_Q_PENDING_HQLOCK_OFFSET + _Q_PENDING_HQLOCK_BITS)
#define _Q_LOCKTYPE_BITS	1
#define _Q_LOCKTYPE_MASK	_Q_SET_MASK(LOCKTYPE)
#define _Q_LOCKTYPE_HQ			(1U << _Q_LOCKTYPE_OFFSET)

#define _Q_LOCK_MODE_OFFSET	(_Q_LOCKTYPE_OFFSET + _Q_LOCKTYPE_BITS)
#define _Q_LOCK_MODE_BITS	2
#define _Q_LOCK_MODE_MASK	_Q_SET_MASK(LOCK_MODE)
#define _Q_LOCK_MODE_QSPINLOCK_VAL	(1U << _Q_LOCK_MODE_OFFSET)

#define _Q_LOCK_TYPE_MODE_MASK	(_Q_LOCKTYPE_MASK | _Q_LOCK_MODE_MASK)
#endif //CONFIG_HQSPINLOCKS

#define _Q_PENDING_MASK		_Q_SET_MASK(PENDING)

#define _Q_TAIL_IDX_OFFSET	(_Q_PENDING_OFFSET + _Q_PENDING_BITS)
#define _Q_TAIL_IDX_BITS	2
#define _Q_TAIL_IDX_MASK	_Q_SET_MASK(TAIL_IDX)

#define _Q_TAIL_CPU_OFFSET	(_Q_TAIL_IDX_OFFSET + _Q_TAIL_IDX_BITS)
#define _Q_TAIL_CPU_BITS	(32 - _Q_TAIL_CPU_OFFSET)
#define _Q_TAIL_CPU_MASK	_Q_SET_MASK(TAIL_CPU)

#define _Q_TAIL_OFFSET		_Q_TAIL_IDX_OFFSET
#define _Q_TAIL_MASK		(_Q_TAIL_IDX_MASK | _Q_TAIL_CPU_MASK)

#define _Q_LOCKED_VAL		(1U << _Q_LOCKED_OFFSET)
#define _Q_PENDING_VAL		(1U << _Q_PENDING_OFFSET)

#ifdef CONFIG_HQSPINLOCKS
#define _Q_LOCK_INVALID_TAIL	(_Q_TAIL_IDX_MASK)

#define _Q_SERVICE_MASK		(_Q_LOCKTYPE_MASK | _Q_LOCK_MODE_QSPINLOCK_VAL | _Q_LOCK_INVALID_TAIL)
#else // CONFIG_HQSPINLOCKS
#define _Q_SERVICE_MASK		0
#endif

/*
 * Initializier
 */
#define	__ARCH_SPIN_LOCK_UNLOCKED		{ { .val = ATOMIC_INIT(0) } }

#ifdef CONFIG_HQSPINLOCKS
#define	__ARCH_SPIN_LOCK_UNLOCKED_HQ	{ { .val = ATOMIC_INIT(_Q_LOCKTYPE_HQ | _Q_LOCK_MODE_QSPINLOCK_VAL) } }
#else
#define	__ARCH_SPIN_LOCK_UNLOCKED_HQ	{ { .val = ATOMIC_INIT(0) } }
#endif

#endif /* __ASM_GENERIC_QSPINLOCK_TYPES_H */
