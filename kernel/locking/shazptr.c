/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simple hazard pointers
 *
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Author: Boqun Feng <boqun.feng@gmail.com>
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/shazptr.h>

DEFINE_PER_CPU_SHARED_ALIGNED(void *, shazptr_slots);
EXPORT_PER_CPU_SYMBOL_GPL(shazptr_slots);

void synchronize_shazptr(void *ptr)
{
	int cpu;

	smp_mb(); /* Synchronize with the smp_mb() in shazptr_acquire(). */
	for_each_possible_cpu(cpu) {
		void **slot = per_cpu_ptr(&shazptr_slots, cpu);
		/* Pair with smp_store_release() in shazptr_clear(). */
		smp_cond_load_acquire(slot,
				      VAL != ptr && VAL != SHAZPTR_WILDCARD);
	}
}
EXPORT_SYMBOL_GPL(synchronize_shazptr);
