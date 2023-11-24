/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_ATOMIC_SEQLOCK_H
#define __LINUX_ATOMIC_SEQLOCK_H

#include <linux/compiler.h>
#include <linux/threads.h>
#include <linux/preempt.h>

/*
 * raw_atomic_seqcount_t -- a reader-writer consistency mechanism with
 * lockless readers (read-only retry loops), and lockless writers.
 * The writers must use atomic RMW operations in the critical section.
 *
 * This locking mechanism is applicable when all individual operations
 * performed by writers can be expressed using atomic RMW operations
 * (so they can run lockless) and readers only need a way to get an atomic
 * view over all individual atomic values: like writers atomically updating
 * multiple counters, and readers wanting to observe a consistent state
 * across all these counters.
 *
 * For now, only the raw variant is implemented, that doesn't perform any
 * lockdep checks.
 *
 * Copyright Red Hat, Inc. 2023
 *
 * Author(s): David Hildenbrand <david@redhat.com>
 */

typedef struct raw_atomic_seqcount {
	atomic_long_t sequence;
} raw_atomic_seqcount_t;

#define raw_seqcount_init(s) atomic_long_set(&((s)->sequence), 0)

#ifdef CONFIG_64BIT

#define ATOMIC_SEQCOUNT_SHARED_WRITER			0x0000000000000001ul
/* 65536 CPUs */
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX		0x0000000000008000ul
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK		0x000000000000fffful
#define ATOMIC_SEQCOUNT_WRITERS_MASK			0x000000000000fffful
/* We have 48bit for the actual sequence. */
#define ATOMIC_SEQCOUNT_SEQUENCE_STEP			0x0000000000010000ul

#else /* CONFIG_64BIT */

#define ATOMIC_SEQCOUNT_SHARED_WRITER			0x00000001ul
/* 64 CPUs */
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX		0x00000040ul
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK		0x0000007ful
#define ATOMIC_SEQCOUNT_WRITERS_MASK			0x0000007ful
/* We have 25bit for the actual sequence. */
#define ATOMIC_SEQCOUNT_SEQUENCE_STEP			0x00000080ul

#endif /* CONFIG_64BIT */

#if CONFIG_NR_CPUS > ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX
#error "raw_atomic_seqcount_t does not support such large CONFIG_NR_CPUS"
#endif

/**
 * raw_read_atomic_seqcount() - read the raw_atomic_seqcount_t counter value
 * @s: Pointer to the raw_atomic_seqcount_t
 *
 * raw_read_atomic_seqcount() opens a read critical section of the given
 * raw_atomic_seqcount_t, and without checking or masking the sequence counter
 * LSBs (using ATOMIC_SEQCOUNT_WRITERS_MASK). Calling code is responsible for
 * handling that.
 *
 * Return: count to be passed to raw_read_atomic_seqcount_retry()
 */
static inline unsigned long raw_read_atomic_seqcount(raw_atomic_seqcount_t *s)
{
	unsigned long seq = atomic_long_read(&s->sequence);

	/* Read the sequence before anything in the critical section */
	smp_rmb();
	return seq;
}

/**
 * raw_read_atomic_seqcount_begin() - begin a raw_seqcount_t read section
 * @s: Pointer to the raw_atomic_seqcount_t
 *
 * raw_read_atomic_seqcount_begin() opens a read critical section of the
 * given raw_seqcount_t. This function must not be used in interrupt context.
 *
 * Return: count to be passed to raw_read_atomic_seqcount_retry()
 */
static inline unsigned long raw_read_atomic_seqcount_begin(raw_atomic_seqcount_t *s)
{
	unsigned long seq;

	BUILD_BUG_ON(IS_ENABLED(CONFIG_PREEMPT_RT));
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON(in_interrupt());
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
	while ((seq = atomic_long_read(&s->sequence)) &
		ATOMIC_SEQCOUNT_WRITERS_MASK)
		cpu_relax();

	/* Load the sequence before any load in the critical section. */
	smp_rmb();
	return seq;
}

/**
 * raw_read_atomic_seqcount_retry() - end a raw_seqcount_t read critical section
 * @s: Pointer to the raw_atomic_seqcount_t
 * @start: count, for example from raw_read_atomic_seqcount_begin()
 *
 * raw_read_atomic_seqcount_retry() closes the read critical section of the
 * given raw_seqcount_t.  If the critical section was invalid, it must be ignored
 * (and typically retried).
 *
 * Return: true if a read section retry is required, else false
 */
static inline bool raw_read_atomic_seqcount_retry(raw_atomic_seqcount_t *s,
		unsigned long start)
{
	/* Load the sequence after any load in the critical section. */
	smp_rmb();
	return unlikely(atomic_long_read(&s->sequence) != start);
}

/**
 * raw_write_seqcount_begin() - start a raw_seqcount_t write critical section
 * @s: Pointer to the raw_atomic_seqcount_t
 *
 * raw_write_seqcount_begin() opens the write critical section of the
 * given raw_seqcount_t. This function must not be used in interrupt context.
 */
static inline void raw_write_atomic_seqcount_begin(raw_atomic_seqcount_t *s)
{
	BUILD_BUG_ON(IS_ENABLED(CONFIG_PREEMPT_RT));
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON(in_interrupt());
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
	preempt_disable();
	atomic_long_add(ATOMIC_SEQCOUNT_SHARED_WRITER, &s->sequence);
	/* Store the sequence before any store in the critical section. */
	smp_mb__after_atomic();
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON((atomic_long_read(&s->sequence) &
			     ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK) >
			    ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX);
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
}

/**
 * raw_write_seqcount_end() - end a raw_seqcount_t write critical section
 * @s: Pointer to the raw_atomic_seqcount_t
 *
 * raw_write_seqcount_end() closes the write critical section of the
 * given raw_seqcount_t.
 */
static inline void raw_write_atomic_seqcount_end(raw_atomic_seqcount_t *s)
{
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON(!(atomic_long_read(&s->sequence) &
			      ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK));
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
	/* Store the sequence after any store in the critical section. */
	smp_mb__before_atomic();
	atomic_long_add(ATOMIC_SEQCOUNT_SEQUENCE_STEP -
			ATOMIC_SEQCOUNT_SHARED_WRITER, &s->sequence);
	preempt_enable();
}

#endif /* __LINUX_ATOMIC_SEQLOCK_H */
