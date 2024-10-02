// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_HP_H
#define _LINUX_HP_H

/*
 * HP: Hazard Pointers
 *
 * This API provides existence guarantees of objects through hazard
 * pointers.
 *
 * It uses a fixed number of hazard pointer slots (nr_cpus) across the
 * entire system for each HP domain.
 *
 * Its main benefit over RCU is that it allows fast reclaim of
 * HP-protected pointers without needing to wait for a grace period.
 *
 * It also allows the hazard pointer scan to call a user-defined callback
 * to retire a hazard pointer slot immediately if needed. This callback
 * may, for instance, issue an IPI to the relevant CPU.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <linux/rcupdate.h>

/*
 * Hazard pointer slot.
 */
struct hp_slot {
	void *addr;
};

/*
 * Hazard pointer context, returned by hp_use().
 */
struct hp_ctx {
	struct hp_slot *slot;
	void *addr;
};

/*
 * hp_scan: Scan hazard pointer domain for @addr.
 *
 * Scan hazard pointer domain for @addr.
 * If @retire_cb is NULL, wait to observe that each slot contains a value
 * that differs from @addr.
 * If @retire_cb is non-NULL, invoke @callback for each slot containing
 * @addr.
 */
void hp_scan(struct hp_slot __percpu *percpu_slots, void *addr,
	     void (*retire_cb)(int cpu, struct hp_slot *slot, void *addr));

/* Get the hazard pointer context address (may be NULL). */
static inline
void *hp_ctx_addr(struct hp_ctx ctx)
{
	return ctx.addr;
}

/*
 * hp_allocate: Allocate a hazard pointer.
 *
 * Allocate a hazard pointer slot for @addr. The object existence should
 * be guaranteed by the caller.
 *
 * Returns a hazard pointer context.
 */
static inline
struct hp_ctx hp_allocate(struct hp_slot __percpu *percpu_slots, void *addr)
{
	struct hp_slot *slot;
	struct hp_ctx ctx;

	if (!addr)
		goto fail;
	slot = this_cpu_ptr(percpu_slots);
	/*
	 * A single hazard pointer slot per CPU is available currently.
	 * Other hazard pointer domains can eventually have a different
	 * configuration.
	 */
	if (READ_ONCE(slot->addr))
		goto fail;
	WRITE_ONCE(slot->addr, addr);	/* Store B */
	ctx.slot = slot;
	ctx.addr = addr;
	return ctx;

fail:
	ctx.slot = NULL;
	ctx.addr = NULL;
	return ctx;
}

/*
 * hp_dereference_allocate: Dereference and allocate a hazard pointer.
 *
 * Returns a hazard pointer context.
 */
static inline
struct hp_ctx hp_dereference_allocate(struct hp_slot __percpu *percpu_slots, void * const * addr_p)
{
	struct hp_slot *slot;
	void *addr, *addr2;
	struct hp_ctx ctx;

	addr = READ_ONCE(*addr_p);
retry:
	ctx = hp_allocate(percpu_slots, addr);
	if (!hp_ctx_addr(ctx))
		goto fail;
	/* Memory ordering: Store B before Load A. */
	smp_mb();
	/*
	 * Use RCU dereference without lockdep checks, because
	 * lockdep is not aware of HP guarantees.
	 */
	addr2 = rcu_access_pointer(*addr_p);	/* Load A */
	/*
	 * If @addr_p content has changed since the first load,
	 * clear the hazard pointer and try again.
	 */
	if (!ptr_eq(addr2, addr)) {
		WRITE_ONCE(slot->addr, NULL);
		if (!addr2)
			goto fail;
		addr = addr2;
		goto retry;
	}
	ctx.slot = slot;
	ctx.addr = addr2;
	return ctx;

fail:
	ctx.slot = NULL;
	ctx.addr = NULL;
	return ctx;
}

/* Retire the hazard pointer in @ctx. */
static inline
void hp_retire(const struct hp_ctx ctx)
{
	smp_store_release(&ctx.slot->addr, NULL);
}

#endif /* _LINUX_HP_H */
