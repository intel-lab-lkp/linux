// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _LINUX_HAZPTR_H
#define _LINUX_HAZPTR_H

/*
 * hazptr: Hazard Pointers
 *
 * This API provides existence guarantees of objects through hazard
 * pointers.
 *
 * Its main benefit over RCU is that it allows fast reclaim of
 * HP-protected pointers without needing to wait for a grace period.
 *
 * References:
 *
 * [1]: M. M. Michael, "Hazard pointers: safe memory reclamation for
 *      lock-free objects," in IEEE Transactions on Parallel and
 *      Distributed Systems, vol. 15, no. 6, pp. 491-504, June 2004
 */

#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/cleanup.h>

/* 8 slots (each sizeof(void *)) fit in a single cache line. */
#define NR_HAZPTR_PERCPU_SLOTS	8

/*
 * Hazard pointer slot.
 */
struct hazptr_slot {
	void *addr;
};

struct hazptr_backup_slot {
	struct list_head node;
	struct hazptr_slot slot;
	/* CPU requesting the backup slot. */
	int cpu;
};

struct hazptr_ctx {
	struct hazptr_slot *slot;
	/* Backup slot in case all per-CPU slots are used. */
	struct hazptr_backup_slot backup_slot;
};

struct hazptr_percpu_slots {
	struct hazptr_slot slots[NR_HAZPTR_PERCPU_SLOTS];
} ____cacheline_aligned;

DECLARE_PER_CPU(struct hazptr_percpu_slots, hazptr_percpu_slots);

/*
 * hazptr_synchronize: Wait until @addr is released from all slots.
 *
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 * Should be called from preemptible context.
 */
void hazptr_synchronize(void *addr);

/*
 * hazptr_chain_backup_slot: Chain backup slot into overflow list.
 *
 * Set backup slot address to @addr, and chain it into the overflow
 * list.
 */
struct hazptr_slot *hazptr_chain_backup_slot(struct hazptr_ctx *ctx);

/*
 * hazptr_unchain_backup_slot: Unchain backup slot from overflow list.
 */
void hazptr_unchain_backup_slot(struct hazptr_ctx *ctx);

static inline
struct hazptr_slot *hazptr_get_free_percpu_slot(void)
{
	struct hazptr_percpu_slots *percpu_slots = this_cpu_ptr(&hazptr_percpu_slots);
	unsigned int idx;

	for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
		struct hazptr_slot *slot = &percpu_slots->slots[idx];

		if (!READ_ONCE(slot->addr))
			return slot;
	}
	/* All slots are in use. */
	return NULL;
}

static inline
bool hazptr_slot_is_backup(struct hazptr_ctx *ctx, struct hazptr_slot *slot)
{
	return slot == &ctx->backup_slot.slot;
}

/*
 * hazptr_acquire: Load pointer at address and protect with hazard pointer.
 *
 * Load @addr_p, and protect the loaded pointer with hazard pointer.
 *
 * Returns a non-NULL protected address if the loaded pointer is non-NULL.
 * Returns NULL if the loaded pointer is NULL.
 *
 * On success the protected hazptr slot is stored in @ctx->slot.
 */
static inline
void *hazptr_acquire(struct hazptr_ctx *ctx, void * const * addr_p)
{
	struct hazptr_slot *slot = NULL;
	void *addr, *addr2;

	/*
	 * Load @addr_p to know which address should be protected.
	 */
	addr = READ_ONCE(*addr_p);
	for (;;) {
		if (!addr)
			return NULL;
		guard(preempt)();
		if (likely(!hazptr_slot_is_backup(ctx, slot))) {
			slot = hazptr_get_free_percpu_slot();
			/*
			 * If all the per-CPU slots are already in use, fallback
			 * to the backup slot.
			 */
			if (unlikely(!slot))
				slot = hazptr_chain_backup_slot(ctx);
		}
		WRITE_ONCE(slot->addr, addr);	/* Store B */

		/* Memory ordering: Store B before Load A. */
		smp_mb();

		/*
		 * Re-load @addr_p after storing it to the hazard pointer slot.
		 */
		addr2 = READ_ONCE(*addr_p);	/* Load A */
		if (likely(ptr_eq(addr2, addr)))
			break;
		/*
		 * If @addr_p content has changed since the first load,
		 * release the hazard pointer and try again.
		 */
		WRITE_ONCE(slot->addr, NULL);
		if (!addr2) {
			if (hazptr_slot_is_backup(ctx, slot))
				hazptr_unchain_backup_slot(ctx);
			return NULL;
		}
		addr = addr2;
	}
	ctx->slot = slot;
	/*
	 * Use addr2 loaded from the second READ_ONCE() to preserve
	 * address dependency ordering.
	 */
	return addr2;
}

/* Release the protected hazard pointer from @slot. */
static inline
void hazptr_release(struct hazptr_ctx *ctx, void *addr)
{
	struct hazptr_slot *slot;

	if (!addr)
		return;
	slot = ctx->slot;
	WARN_ON_ONCE(slot->addr != addr);
	smp_store_release(&slot->addr, NULL);
	if (unlikely(hazptr_slot_is_backup(ctx, slot)))
		hazptr_unchain_backup_slot(ctx);
}

void hazptr_init(void);

#endif /* _LINUX_HAZPTR_H */
