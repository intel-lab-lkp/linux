// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_HAZPTR_H
#define _LINUX_HAZPTR_H

/*
 * HP: Hazard Pointers
 *
 * This API provides existence guarantees of objects through hazard
 * pointers.
 *
 * It uses a fixed number of hazard pointer slots (nr_cpus) across the
 * entire system for each hazard pointer domain.
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

#include <linux/percpu.h>
#include <linux/types.h>

/*
 * Hazard pointer slot.
 */
struct hazptr_slot {
	void *addr;
};

struct hazptr_domain {
	struct hazptr_slot __percpu *percpu_slots;
};

#define DECLARE_HAZPTR_DOMAIN(domain)					\
	extern struct hazptr_domain domain

#define DEFINE_HAZPTR_DOMAIN(domain)					\
	static DEFINE_PER_CPU(struct hazptr_slot, __ ## domain ## _slots); \
	struct hazptr_domain domain = {					\
		.percpu_slots = &__## domain ## _slots,			\
	}

/*
 * hazptr_scan: Scan hazard pointer domain for @addr.
 *
 * Scan hazard pointer domain for @addr.
 * If @on_match_cb is NULL, wait to observe that each slot contains a value
 * that differs from @addr.
 * If @on_match_cb is non-NULL, invoke @on_match_cb for each slot containing
 * @addr.
 */
void hazptr_scan(struct hazptr_domain *domain, void *addr,
	     void (*on_match_cb)(int cpu, struct hazptr_slot *slot, void *addr));

/*
 * hazptr_try_protect: Try to protect with hazard pointer.
 *
 * Try to protect @addr with a hazard pointer slot. The object existence
 * should be guaranteed by the caller. Expects to be called from preempt
 * disable context.
 *
 * Returns true if protect succeeds, false otherwise.
 * On success, if @_slot is not NULL, the protected hazptr slot is stored in @_slot.
 */
static inline
bool hazptr_try_protect(struct hazptr_domain *hazptr_domain, void *addr, struct hazptr_slot **_slot)
{
	struct hazptr_slot __percpu *percpu_slots = hazptr_domain->percpu_slots;
	struct hazptr_slot *slot;

	if (!addr)
		return false;
	slot = this_cpu_ptr(percpu_slots);
	/*
	 * A single hazard pointer slot per CPU is available currently.
	 * Other hazard pointer domains can eventually have a different
	 * configuration.
	 */
	if (READ_ONCE(slot->addr))
		return false;
	WRITE_ONCE(slot->addr, addr);	/* Store B */
	if (_slot)
		*_slot = slot;
	return true;
}

/*
 * hazptr_load_try_protect: Load and try to protect with hazard pointer.
 *
 * Load @addr_p, and try to protect the loaded pointer with hazard
 * pointers.
 *
 * Returns a protected address on success, NULL on failure. Expects to
 * be called from preempt disable context.
 *
 * On success, if @_slot is not NULL, the protected hazptr slot is stored in @_slot.
 */
static inline
void *__hazptr_load_try_protect(struct hazptr_domain *hazptr_domain,
			 void * const * addr_p, struct hazptr_slot **_slot)
{
	struct hazptr_slot *slot;
	void *addr, *addr2;

	/*
	 * Load @addr_p to know which address should be protected.
	 */
	addr = READ_ONCE(*addr_p);
retry:
	/* Try to protect the address by storing it into a slot. */
	if (!hazptr_try_protect(hazptr_domain, addr, &slot))
		return NULL;
	/* Memory ordering: Store B before Load A. */
	smp_mb();
	/*
	 * Re-load @addr_p after storing it to the hazard pointer slot.
	 */
	addr2 = READ_ONCE(*addr_p);	/* Load A */
	/*
	 * If @addr_p content has changed since the first load,
	 * retire the hazard pointer and try again.
	 */
	if (!ptr_eq(addr2, addr)) {
		WRITE_ONCE(slot->addr, NULL);
		if (!addr2)
			return NULL;
		addr = addr2;
		goto retry;
	}
	if (_slot)
		*_slot = slot;
	/*
	 * Use addr2 loaded from the second READ_ONCE() to preserve
	 * address dependency ordering.
	 */
	return addr2;
}

/*
 * Use a comma expression within typeof: __typeof__((void)**(addr_p), *(addr_p))
 * to generate a compile error if addr_p is not a pointer to a pointer.
 */
#define hazptr_load_try_protect(domain, addr_p, slot_p)	\
	((__typeof__((void)**(addr_p), *(addr_p))) __hazptr_load_try_protect(domain, (void * const *) (addr_p), slot_p))

/* Retire the protected hazard pointer from @slot. */
static inline
void hazptr_retire(struct hazptr_slot *slot, void *addr)
{
	WARN_ON_ONCE(slot->addr != addr);
	smp_store_release(&slot->addr, NULL);
}

#endif /* _LINUX_HAZPTR_H */
