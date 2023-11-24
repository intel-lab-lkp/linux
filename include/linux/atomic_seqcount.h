/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __LINUX_ATOMIC_SEQLOCK_H
#define __LINUX_ATOMIC_SEQLOCK_H

#include <linux/compiler.h>
#include <linux/threads.h>
#include <linux/preempt.h>

/*
 * raw_atomic_seqcount_t -- a reader-writer consistency mechanism with
 * lockless readers (read-only retry loops), and (almost) lockless writers.
 * Shared writers must use atomic RMW operations in the critical section,
 * a single exclusive writer can avoid atomic RMW operations in the critical
 * section. Shared writers will always have to wait for at most one exclusive
 * writer to finish in order to make progress.
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
#define ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER		0x0000000000010000ul
#define ATOMIC_SEQCOUNT_WRITERS_MASK			0x000000000001fffful
/* We have 48bit for the actual sequence. */
#define ATOMIC_SEQCOUNT_SEQUENCE_STEP			0x0000000000020000ul

#else /* CONFIG_64BIT */

#define ATOMIC_SEQCOUNT_SHARED_WRITER			0x00000001ul
/* 64 CPUs */
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX		0x00000040ul
#define ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK		0x0000007ful
#define ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER		0x00000080ul
#define ATOMIC_SEQCOUNT_WRITERS_MASK			0x000000fful
/* We have 24bit for the actual sequence. */
#define ATOMIC_SEQCOUNT_SEQUENCE_STEP			0x00000100ul

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
 * @try_exclusive: Whether to try becoming the exclusive writer.
 *
 * raw_write_seqcount_begin() opens the write critical section of the
 * given raw_seqcount_t. This function must not be used in interrupt context.
 *
 * Return: "true" when we are the exclusive writer and can avoid atomic RMW
 *         operations in the critical section. Otherwise, we are a shared
 *         writer and have to use atomic RMW operations in the critical
 *         section. Will always return "false" if @try_exclusive is not "true".
 */
static inline bool raw_write_atomic_seqcount_begin(raw_atomic_seqcount_t *s,
						   bool try_exclusive)
{
	unsigned long seqcount, seqcount_new;

	BUILD_BUG_ON(IS_ENABLED(CONFIG_PREEMPT_RT));
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON(in_interrupt());
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
	preempt_disable();

	/* If requested, can we just become the exclusive writer? */
	if (!try_exclusive)
		goto shared;

	seqcount = atomic_long_read(&s->sequence);
	if (unlikely(seqcount & ATOMIC_SEQCOUNT_WRITERS_MASK))
		goto shared;

	seqcount_new = seqcount | ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER;
	/*
	 * Store the sequence before any store in the critical section. Further,
	 * this implies an acquire so loads within the critical section are
	 * not reordered to be outside the critical section.
	 */
	if (atomic_long_try_cmpxchg(&s->sequence, &seqcount, seqcount_new))
		return true;
shared:
	/*
	 * Indicate that there is a shared writer, and spin until the exclusive
	 * writer is done. This avoids writer starvation, because we'll always
	 * have to wait for at most one writer.
	 *
	 * We spin with preemption disabled to not reschedule to a reader that
	 * cannot make any progress either way.
	 *
	 * Store the sequence before any store in the critical section.
	 */
	seqcount = atomic_long_add_return(ATOMIC_SEQCOUNT_SHARED_WRITER,
					  &s->sequence);
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
	DEBUG_LOCKS_WARN_ON((seqcount & ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK) >
			    ATOMIC_SEQCOUNT_SHARED_WRITERS_MAX);
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
	if (likely(!(seqcount & ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER)))
		return false;

	while (atomic_long_read(&s->sequence) & ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER)
		cpu_relax();
	return false;
}

/**
 * raw_write_seqcount_end() - end a raw_seqcount_t write critical section
 * @s: Pointer to the raw_atomic_seqcount_t
 * @exclusive: Return value of raw_write_atomic_seqcount_begin().
 *
 * raw_write_seqcount_end() closes the write critical section of the
 * given raw_seqcount_t.
 */
static inline void raw_write_atomic_seqcount_end(raw_atomic_seqcount_t *s,
						 bool exclusive)
{
	unsigned long val = ATOMIC_SEQCOUNT_SEQUENCE_STEP;

	if (likely(exclusive)) {
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
		DEBUG_LOCKS_WARN_ON(!(atomic_long_read(&s->sequence) &
				      ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER));
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
		val -= ATOMIC_SEQCOUNT_EXCLUSIVE_WRITER;
	} else {
#ifdef CONFIG_DEBUG_ATOMIC_SEQCOUNT
		DEBUG_LOCKS_WARN_ON(!(atomic_long_read(&s->sequence) &
				      ATOMIC_SEQCOUNT_SHARED_WRITERS_MASK));
#endif /* CONFIG_DEBUG_ATOMIC_SEQCOUNT */
		val -= ATOMIC_SEQCOUNT_SHARED_WRITER;
	}
	/*
	 * Store the sequence after any store in the critical section. For
	 * the exclusive path, this further implies a release, so loads
	 * within the critical section are not reordered to be outside the
	 * cricial section.
	 */
	smp_mb__before_atomic();
	atomic_long_add(val, &s->sequence);
	preempt_enable();
}

#endif /* __LINUX_ATOMIC_SEQLOCK_H */
