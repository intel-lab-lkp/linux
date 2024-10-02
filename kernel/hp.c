// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * HP: Hazard Pointers
 */

#include <linux/hp.h>
#include <linux/percpu.h>

/*
 * hp_scan: Scan hazard pointer domain for @addr.
 *
 * Scan hazard pointer domain for @addr.
 * If @retire_cb is non-NULL, invoke @callback for each slot containing
 * @addr.
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 */
void hp_scan(struct hp_slot __percpu *percpu_slots, void *addr,
	     void (*retire_cb)(int cpu, struct hp_slot *slot, void *addr))
{
	int cpu;

	/*
	 * Store A precedes hp_scan(): it unpublishes addr (sets it to
	 * NULL or to a different value), and thus hides it from hazard
	 * pointer readers.
	 */

	if (!addr)
		return;
	/* Memory ordering: Store A before Load B. */
	smp_mb();
	/* Scan all CPUs slots. */
	for_each_possible_cpu(cpu) {
		struct hp_slot *slot = per_cpu_ptr(percpu_slots, cpu);

		if (retire_cb && smp_load_acquire(&slot->addr) == addr)	/* Load B */
			retire_cb(cpu, slot, addr);
		/* Busy-wait if node is found. */
		while ((smp_load_acquire(&slot->addr)) == addr)	/* Load B */
			cpu_relax();
	}
}
