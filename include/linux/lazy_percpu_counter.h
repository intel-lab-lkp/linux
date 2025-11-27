/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/percpu_counter.h>
#ifndef _LAZY_PERCPU_COUNTER
#define _LAZY_PERCPU_COUNTER

/* Lazy percpu counter is a bi-modal distributed counter structure that
 * starts off as a simple counter and can be upgraded to a full per-cpu
 * counter when the user considers more non-local updates are likely to
 * happen more frequently in the future.  It is useful when non-local
 * updates are rare, but might become more frequent after other
 * operations.
 *
 * - Lazy-mode:
 *
 * Local updates are handled with a simple variable write, while
 * non-local updates are handled through an atomic operation.  Once
 * non-local updates become more likely to happen in the future, the
 * user can upgrade the counter, turning it into a normal
 * per-cpu counter.
 *
 * Concurrency safety of 'local' accesses must be guaranteed by the
 * caller API, either through task-local accesses or by external locks.
 *
 * In the initial lazy-mode, read is guaranteed to be exact only when
 * reading from the local context with lazy_percpu_counter_sum_local.
 *
 * - Non-lazy-mode:
 *   Behaves as a per-cpu counter.
 */

struct lazy_percpu_counter {
	struct percpu_counter c;
};

#define LAZY_INIT_BIAS (1<<0)

static inline s64 add_bias(long val)
{
	return (val << 1) | LAZY_INIT_BIAS;
}
static inline s64 remove_bias(long val)
{
	return val >> 1;
}

static inline bool lazy_percpu_counter_initialized(struct lazy_percpu_counter *lpc)
{
	return !(atomic_long_read(&lpc->c.remote) & LAZY_INIT_BIAS);
}

static inline void lazy_percpu_counter_init_many(struct lazy_percpu_counter *lpc, int amount,
					       int nr_counters)
{
	for (int i = 0; i < nr_counters; i++) {
		lpc[i].c.count = amount;
		atomic_long_set(&lpc[i].c.remote, LAZY_INIT_BIAS);
		raw_spin_lock_init(&lpc[i].c.lock);
	}
}

static inline void lazy_percpu_counter_add_atomic(struct lazy_percpu_counter *lpc, s64 amount)
{
	long x = amount << 1;
	long counter;

	do {
		counter = atomic_long_read(&lpc->c.remote);
		if (!(counter & LAZY_INIT_BIAS)) {
			percpu_counter_add(&lpc->c, amount);
			return;
		}
	} while (atomic_long_cmpxchg_relaxed(&lpc->c.remote, counter, (counter+x)) != counter);
}

static inline void lazy_percpu_counter_add_fast(struct lazy_percpu_counter *lpc, s64 amount)
{
	if (lazy_percpu_counter_initialized(lpc))
		percpu_counter_add(&lpc->c, amount);
	else
		lpc->c.count += amount;
}

/*
 * lazy_percpu_counter_sync needs to be protected against concurrent
 * local updates.
 */
static inline s64 lazy_percpu_counter_sum_local(struct lazy_percpu_counter *lpc)
{
	if (lazy_percpu_counter_initialized(lpc))
		return percpu_counter_sum(&lpc->c);

	lazy_percpu_counter_add_atomic(lpc, lpc->c.count);
	lpc->c.count = 0;
	return remove_bias(atomic_long_read(&lpc->c.remote));
}

static inline s64 lazy_percpu_counter_sum(struct lazy_percpu_counter *lpc)
{
	if (lazy_percpu_counter_initialized(lpc))
		return percpu_counter_sum(&lpc->c);
	return remove_bias(atomic_long_read(&lpc->c.remote)) + lpc->c.count;
}

static inline s64 lazy_percpu_counter_sum_positive(struct lazy_percpu_counter *lpc)
{
	s64 val = lazy_percpu_counter_sum(lpc);

	return (val > 0) ? val : 0;
}

static inline s64 lazy_percpu_counter_read(struct lazy_percpu_counter *lpc)
{
	if (lazy_percpu_counter_initialized(lpc))
		return percpu_counter_read(&lpc->c);
	return remove_bias(atomic_long_read(&lpc->c.remote)) + lpc->c.count;
}

static inline s64 lazy_percpu_counter_read_positive(struct lazy_percpu_counter *lpc)
{
	s64 val = lazy_percpu_counter_read(lpc);

	return (val > 0) ? val : 0;
}

int __lazy_percpu_counter_upgrade_many(struct lazy_percpu_counter *c,
				       int nr_counters, gfp_t gfp);
static inline int lazy_percpu_counter_upgrade_many(struct lazy_percpu_counter *c,
						   int nr_counters, gfp_t gfp)
{
	/* Only check the first element, as batches are expected to be
	 * upgraded together.
	 */
	if (!lazy_percpu_counter_initialized(c))
		return __lazy_percpu_counter_upgrade_many(c, nr_counters, gfp);
	return 0;
}

static inline void lazy_percpu_counter_destroy_many(struct lazy_percpu_counter *lpc,
						    u32 nr_counters)
{
	/* Only check the first element, as they must have been initialized together. */
	if (lazy_percpu_counter_initialized(lpc))
		percpu_counter_destroy_many((struct percpu_counter *)lpc, nr_counters);
}
#endif
