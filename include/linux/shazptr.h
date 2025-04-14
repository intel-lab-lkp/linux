/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simple hazard pointers
 *
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Author: Boqun Feng <boqun.feng@gmail.com>
 *
 * A simple variant of hazard pointers, the users must ensure the preemption
 * is already disabled when calling a shazptr_acquire() to protect an address.
 * If one shazptr_acquire() is called after another shazptr_acquire() has been
 * called without the corresponding shazptr_clear() has been called, the later
 * shazptr_acquire() must be cleared first.
 *
 * The most suitable usage is when only one address need to be protected in a
 * preemption disabled critical section.
 */

#ifndef _LINUX_SHAZPTR_H
#define _LINUX_SHAZPTR_H

#include <linux/cleanup.h>
#include <linux/percpu.h>

/* Make ULONG_MAX the wildcard value */
#define SHAZPTR_WILDCARD ((void *)(ULONG_MAX))

DECLARE_PER_CPU_SHARED_ALIGNED(void *, shazptr_slots);

/* Represent a held hazard pointer slot */
struct shazptr_guard {
	void **slot;
	bool use_wildcard;
};

/*
 * Acquire a hazptr slot and begin the hazard pointer critical section.
 *
 * Must be called with preemption disabled, and preemption must remain disabled
 * until shazptr_clear().
 */
static inline struct shazptr_guard shazptr_acquire(void *ptr)
{
	struct shazptr_guard guard = {
		/* Preemption is disabled. */
		.slot = this_cpu_ptr(&shazptr_slots),
		.use_wildcard = false,
	};

	if (likely(!READ_ONCE(*guard.slot))) {
		WRITE_ONCE(*guard.slot, ptr);
	} else {
		guard.use_wildcard = true;
		WRITE_ONCE(*guard.slot, SHAZPTR_WILDCARD);
	}

	smp_mb(); /* Synchronize with smp_mb() at synchronize_shazptr(). */

	return guard;
}

static inline void shazptr_clear(struct shazptr_guard guard)
{
	/* Only clear the slot when the outermost guard is released */
	if (likely(!guard.use_wildcard))
		smp_store_release(guard.slot, NULL); /* Pair with ACQUIRE at synchronize_shazptr() */
}

void synchronize_shazptr(void *ptr);

DEFINE_CLASS(shazptr, struct shazptr_guard, shazptr_clear(_T),
	     shazptr_acquire(ptr), void *ptr);
#endif
