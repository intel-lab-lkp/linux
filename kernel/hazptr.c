// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * hazptr: Hazard Pointers
 */

#include <linux/hazptr.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/export.h>

struct overflow_list {
	raw_spinlock_t lock;		/* Lock protecting overflow list and list generation. */
	struct list_head head;		/* Overflow list head. */
	uint64_t gen;			/* Overflow list generation. */
};

static DEFINE_PER_CPU(struct overflow_list, percpu_overflow_list);

DEFINE_PER_CPU(struct hazptr_percpu_slots, hazptr_percpu_slots);
EXPORT_PER_CPU_SYMBOL_GPL(hazptr_percpu_slots);

/*
 * Perform piecewise iteration on overflow list waiting until "addr" is
 * not present. Raw spinlock is released and taken between each list
 * item and busy loop iteration. The overflow list generation is checked
 * each time the lock is taken to validate that the list has not changed
 * before resuming iteration or busy wait. If the generation has
 * changed, retry the entire list traversal.
 */
static
void hazptr_synchronize_overflow_list(struct overflow_list *overflow_list, void *addr)
{
	struct hazptr_backup_slot *backup_slot;
	uint64_t snapshot_gen;

	raw_spin_lock(&overflow_list->lock);
retry:
	snapshot_gen = overflow_list->gen;
	list_for_each_entry(backup_slot, &overflow_list->head, node) {
		/* Busy-wait if node is found. */
		while (smp_load_acquire(&backup_slot->slot.addr) == addr) { /* Load B */
			raw_spin_unlock(&overflow_list->lock);
			cpu_relax();
			raw_spin_lock(&overflow_list->lock);
			if (overflow_list->gen != snapshot_gen)
				goto retry;
		}
		raw_spin_unlock(&overflow_list->lock);
		/*
		 * Release raw spinlock, validate generation after
		 * re-acquiring the lock.
		 */
		raw_spin_lock(&overflow_list->lock);
		if (overflow_list->gen != snapshot_gen)
			goto retry;
	}
	raw_spin_unlock(&overflow_list->lock);
}

static
void hazptr_synchronize_cpu_slots(int cpu, void *addr)
{
	struct hazptr_percpu_slots *percpu_slots = per_cpu_ptr(&hazptr_percpu_slots, cpu);
	unsigned int idx;

	for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
		struct hazptr_slot *slot = &percpu_slots->slots[idx];

		/* Busy-wait if node is found. */
		smp_cond_load_acquire(&slot->addr, VAL != addr); /* Load B */
	}
}

/*
 * hazptr_synchronize: Wait until @addr is released from all slots.
 *
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 * Should be called from preemptible context.
 */
void hazptr_synchronize(void *addr)
{
	int cpu;

	/*
	 * Busy-wait should only be done from preemptible context.
	 */
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
		/* Scan CPU slots. */
		hazptr_synchronize_cpu_slots(cpu, addr);
		/* Scan backup slots in percpu overflow list. */
		hazptr_synchronize_overflow_list(per_cpu_ptr(&percpu_overflow_list, cpu), addr);
	}
}
EXPORT_SYMBOL_GPL(hazptr_synchronize);

struct hazptr_slot *hazptr_chain_backup_slot(struct hazptr_ctx *ctx)
{
	struct overflow_list *overflow_list = this_cpu_ptr(&percpu_overflow_list);
	struct hazptr_slot *slot = &ctx->backup_slot.slot;

	slot->addr = NULL;

	raw_spin_lock(&overflow_list->lock);
	overflow_list->gen++;
	list_add(&ctx->backup_slot.node, &overflow_list->head);
	ctx->backup_slot.cpu = smp_processor_id();
	raw_spin_unlock(&overflow_list->lock);
	return slot;
}
EXPORT_SYMBOL_GPL(hazptr_chain_backup_slot);

void hazptr_unchain_backup_slot(struct hazptr_ctx *ctx)
{
	struct overflow_list *overflow_list = per_cpu_ptr(&percpu_overflow_list, ctx->backup_slot.cpu);

	raw_spin_lock(&overflow_list->lock);
	overflow_list->gen++;
	list_del(&ctx->backup_slot.node);
	raw_spin_unlock(&overflow_list->lock);
}
EXPORT_SYMBOL_GPL(hazptr_unchain_backup_slot);

void __init hazptr_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct overflow_list *overflow_list = per_cpu_ptr(&percpu_overflow_list, cpu);

		raw_spin_lock_init(&overflow_list->lock);
		INIT_LIST_HEAD(&overflow_list->head);
	}
}
