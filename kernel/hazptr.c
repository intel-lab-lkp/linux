// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * hazptr: Hazard Pointers
 */

#include <linux/hazptr.h>
#include <linux/percpu.h>

/*
 * hazptr_scan: Scan hazard pointer domain for @addr.
 *
 * Scan hazard pointer domain for @addr.
 * If @on_match_cb is non-NULL, invoke @callback for each slot containing
 * @addr.
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 */
void hazptr_scan(struct hazptr_domain *hazptr_domain, void *addr,
	     void (*on_match_cb)(int cpu, struct hazptr_slot *slot, void *addr))
{
	struct hazptr_slot __percpu *percpu_slots = hazptr_domain->percpu_slots;
	int cpu;

	/* Should only be called from preemptible context. */
	lockdep_assert_preemption_enabled();

	/*
	 * Store A precedes hazptr_scan(): it unpublishes addr (sets it to
	 * NULL or to a different value), and thus hides it from hazard
	 * pointer readers.
	 */
	if (!addr)
		return;
	/* Memory ordering: Store A before Load B. */
	smp_mb();
	/* Scan all CPUs slots. */
	for_each_possible_cpu(cpu) {
		struct hazptr_slot *slot = per_cpu_ptr(percpu_slots, cpu);

		if (on_match_cb) {
			if (smp_load_acquire(&slot->addr) == addr)	/* Load B */
				on_match_cb(cpu, slot, addr);
		} else {
			/* Busy-wait if node is found. */
			smp_cond_load_acquire(&slot->addr, VAL != addr); /* Load B */
		}
	}
}
