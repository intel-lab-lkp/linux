// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SPDX-FileCopyrightText: 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>

/*
 * hazptr: Hazard Pointers
 */

#include <linux/hazptr.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/swait.h>

/*
 * The current hazard pointer wildcard. Flips between 1UL and 2UL to guarantee
 * hazptr_synchronize forward progress even with a steady stream of readers.
 * This wildcard value is used by acquire to temporarily tag the per-CPU slots.
 * This also affects the overflow list selection: the current list used by
 * readers is array[(unsigned long) hazptr_wildcard - 1].
 */
static DEFINE_MUTEX(hazptr_wildcard_lock);	/* Protect the wildcard flip. */
void *hazptr_wildcard = (void *) 1UL;
EXPORT_SYMBOL_GPL(hazptr_wildcard);

struct hazptr_overflow_list {
	raw_spinlock_t lock;		/* Lock protecting overflow list and list generation. */
	struct hlist_head head;		/* Overflow list head. */
	uint64_t gen;			/* Overflow list generation. */
};

/*
 * Flip between two lists to guarantee list scan forward progress even
 * with frequent generation counter increments. The list additions are
 * always done on a different list than the one used for scan. The scan
 * successively iterates on both lists. Therefore, only list removals
 * can cause the iteration to retry, and the number of removals is
 * limited to the number of list elements.
 */
struct hazptr_overflow_list_flip {
	struct hazptr_overflow_list array[2];
};

static DEFINE_PER_CPU(struct hazptr_overflow_list_flip, percpu_overflow_list_flip);

DEFINE_PER_CPU(struct hazptr_percpu_slots, hazptr_percpu_slots);
EXPORT_PER_CPU_SYMBOL_GPL(hazptr_percpu_slots);

static
void *flip_wildcard(void *wildcard)
{
	return ((unsigned long) wildcard == 1UL) ? (void *) 2UL : (void *) 1UL;
}

static
bool is_wildcard(void *addr)
{
	if ((unsigned long) addr == 1UL || (unsigned long) addr == 2UL)
		return true;
	return false;
}

static
struct hazptr_slot *hazptr_get_free_percpu_slot(struct hazptr_ctx *ctx)
{
	struct hazptr_percpu_slots *percpu_slots = this_cpu_ptr(&hazptr_percpu_slots);
	unsigned int idx;

	for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
		struct hazptr_slot_item *item = &percpu_slots->items[idx];
		struct hazptr_slot *slot = &item->slot;

		if (!slot->addr) {
			item->ctx.ctx = ctx;
			return slot;
		}
	}
	/* All slots are in use. */
	return NULL;
}

/*
 * Hazard pointer acquire slow path.
 * Called with preemption disabled.
 */
void *__hazptr_acquire(struct hazptr_ctx *ctx, void * const *addr_p)
{
	struct hazptr_slot *slot = hazptr_get_free_percpu_slot(ctx);
	void *addr;

	/*
	 * If all the per-CPU slots are already in use, fallback
	 * to the backup slot.
	 */
	if (unlikely(!slot))
		slot = hazptr_chain_backup_slot(ctx);
	WRITE_ONCE(slot->addr, READ_ONCE(hazptr_wildcard));	/* Store B */

	/* Memory ordering: Store B before Load A. */
	smp_mb();

	/*
	 * Load @addr_p after storing wildcard to the hazard pointer slot.
	 */
	addr = READ_ONCE(*addr_p);	/* Load A */

	/*
	 * We don't care about ordering of Store C. It will simply
	 * replace the wildcard by a more specific address. If addr is
	 * NULL, we simply store NULL into the slot.
	 */
	WRITE_ONCE(slot->addr, addr);	/* Store C */
	ctx->slot = slot;
	if (!addr && hazptr_slot_is_backup(ctx, slot))
		hazptr_unchain_backup_slot(ctx);
	return addr;
}
EXPORT_SYMBOL_GPL(__hazptr_acquire);

/*
 * Perform piecewise iteration on overflow list waiting until "addr" is
 * not present. Raw spinlock is released and taken between each list
 * item and busy loop iteration. The overflow list generation is checked
 * each time the lock is taken to validate that the list has not changed
 * before resuming iteration or busy wait. If the generation has
 * changed, retry the entire list traversal.
 */
static
void hazptr_synchronize_overflow_list(struct hazptr_overflow_list *overflow_list, void *addr)
{
	struct hazptr_backup_slot *backup_slot;
	uint64_t snapshot_gen;
	unsigned long flags;

	raw_spin_lock_irqsave(&overflow_list->lock, flags);
retry:
	snapshot_gen = overflow_list->gen;
	hlist_for_each_entry(backup_slot, &overflow_list->head, overflow_node) {
		/* Busy-wait if node is found. */
		for (;;) {
			void *load_addr = smp_load_acquire(&backup_slot->slot.addr);	/* Load B */

			/* We don't expect wildcards in overflow list. */
			WARN_ON_ONCE(is_wildcard(load_addr));
			if (load_addr != addr)
				break;
			raw_spin_unlock_irqrestore(&overflow_list->lock, flags);
			cpu_relax();
			raw_spin_lock_irqsave(&overflow_list->lock, flags);
			if (overflow_list->gen != snapshot_gen)
				goto retry;
		}
		raw_spin_unlock_irqrestore(&overflow_list->lock, flags);
		/*
		 * Release raw spinlock, validate generation after
		 * re-acquiring the lock.
		 */
		raw_spin_lock_irqsave(&overflow_list->lock, flags);
		if (overflow_list->gen != snapshot_gen)
			goto retry;
	}
	raw_spin_unlock_irqrestore(&overflow_list->lock, flags);
}

static
void hazptr_synchronize_cpu_slots(int cpu, void *addr, void *scan_wildcard)
{
	struct hazptr_percpu_slots *percpu_slots = per_cpu_ptr(&hazptr_percpu_slots, cpu);
	unsigned int idx;

	for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
		struct hazptr_slot_item *item = &percpu_slots->items[idx];

		/* Busy-wait if node is found. */
		smp_cond_load_acquire(&item->slot.addr, VAL != addr && VAL != scan_wildcard); /* Load B */
	}
}

static
void hazptr_scan_period(void *addr, void *scan_wildcard)
{
	unsigned int scan_idx = (unsigned long) scan_wildcard - 1;
	int cpu;

	/* Scan all CPUs slots. */
	for_each_possible_cpu(cpu) {
		struct hazptr_overflow_list_flip *overflow_list_flip = per_cpu_ptr(&percpu_overflow_list_flip, cpu);

		/*
		 * Scan CPU slots.
		 * Forward progress against recurring wildcards is guaranteed
		 * by scanning for one wildcard while new elements use the
		 * other wildcard value (1UL vs 2UL).
		 * Forward progress against recurring single hazard pointer
		 * values is guaranteed by the fact that a hazard pointer
		 * is not reclaimed nor reused until the scan for that hazard
		 * pointer completes, which prevents a steady flow of readers
		 * to acquire that same hazard pointer value.
		 */
		hazptr_synchronize_cpu_slots(cpu, addr, scan_wildcard);

		/*
		 * Scan backup slots in percpu overflow lists.
		 * Forward progress is guaranteed by scanning one list
		 * while new elements are added into the other list.
		 */
		hazptr_synchronize_overflow_list(&overflow_list_flip->array[scan_idx], addr);
	}
}

/*
 * Batch hazptr_synchronize() callers through a shared scan kthread.
 */

struct hazptr_waiter {
	struct list_head node;
	void *addr;
	struct completion done;
	/*
	 * Per-wildcard-generation progress masks.  A CPU bit is
	 * cleared when the scan observes neither @addr nor that
	 * generation's wildcard on the CPU.
	 */
	unsigned long *cpu_mask;	/* 2 * BITS_TO_LONGS(nr_cpu_ids) */
};

/* Return waiter @w's progress mask for wildcard generation @gen. */
static unsigned long *hazptr_waiter_mask(struct hazptr_waiter *w, int gen)
{
	return w->cpu_mask + gen * BITS_TO_LONGS(nr_cpu_ids);
}

struct hazptr_scan_state {
	struct task_struct *kthread;
	struct swait_queue_head wq;
	bool wakeup;
	struct mutex lock;
	struct list_head pending;
	struct list_head scanning;	/* kthread only */
};
static struct hazptr_scan_state hazptr_scan;

/*
 * Check a CPU's overflow lists.  A backup slot can hold a wildcard
 * because __hazptr_acquire() writes the wildcard to any slot,
 * including backup slots from hazptr_chain_backup_slot().
 *
 * @addr:     address the waiter is waiting on
 * @old_wc:   wildcard value of the pre-flip generation
 * @new_wc:   wildcard value of the post-flip generation
 * @has_old:  set if any overflow slot holds @old_wc
 * @has_new:  set if any overflow slot holds @new_wc
 *
 * Returns true if @addr is present.
 */
static bool hazptr_ovf_list_blocked(int cpu, void *addr,
				    void *old_wc, void *new_wc,
				    bool *has_old, bool *has_new)
{
	struct hazptr_overflow_list_flip *ovf = per_cpu_ptr(&percpu_overflow_list_flip, cpu);
	bool found_addr = false;
	int i;

	for (i = 0; i < 2; i++) {
		struct hazptr_overflow_list *list = &ovf->array[i];
		struct hazptr_backup_slot *b;
		unsigned long flags;

		raw_spin_lock_irqsave(&list->lock, flags);
		hlist_for_each_entry(b, &list->head, overflow_node) {
			/* Pairs with smp_store_release in hazptr_release(). */
			void *val = smp_load_acquire(&b->slot.addr);

			if (val == addr)
				found_addr = true;
			else if (val == old_wc)
				*has_old = true;
			else if (val == new_wc)
				*has_new = true;
		}
		raw_spin_unlock_irqrestore(&list->lock, flags);
	}
	return found_addr;
}

/*
 * Move pending waiters to ->scanning, flip the wildcard, then make
 * one pass over all CPUs.  Clear per-waiter bits for CPUs that no
 * longer hold the waiter address or the corresponding wildcard.
 *
 * After the flip, new acquires use the new wildcard.  The old
 * generation therefore makes forward progress and is fully cleared
 * after enough scan cycles.
 */
static void hazptr_scan_do_cycle(void)
{
	void *old_wc, *new_wc;
	unsigned int old_idx, new_idx;
	int cpu;
	struct hazptr_waiter *w, *n;
	LIST_HEAD(done);

	mutex_lock(&hazptr_wildcard_lock);

	mutex_lock(&hazptr_scan.lock);
	list_splice_tail_init(&hazptr_scan.pending, &hazptr_scan.scanning);
	mutex_unlock(&hazptr_scan.lock);

	if (list_empty(&hazptr_scan.scanning)) {
		mutex_unlock(&hazptr_wildcard_lock);
		return;
	}

	old_wc = READ_ONCE(hazptr_wildcard);
	new_wc = flip_wildcard(old_wc);
	WRITE_ONCE(hazptr_wildcard, new_wc);
	old_idx = (unsigned long)old_wc - 1;
	new_idx = 1 - old_idx;

	/*
	 * One pass over all CPUs for the per-CPU slots, checking
	 * overflow lists for the remaining waiters.
	 */
	for_each_possible_cpu(cpu) {
		struct hazptr_percpu_slots *slots = per_cpu_ptr(&hazptr_percpu_slots, cpu);
		void *vals[NR_HAZPTR_PERCPU_SLOTS];
		bool has_old = false, has_new = false;
		unsigned int idx;

		for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
			/* Pairs with smp_store_release in hazptr_release(). */
			vals[idx] = smp_load_acquire(&slots->items[idx].slot.addr);
			if (vals[idx] == old_wc)
				has_old = true;
			else if (vals[idx] == new_wc)
				has_new = true;
		}

		list_for_each_entry(w, &hazptr_scan.scanning, node) {
			bool has_addr = false;

			if (!test_bit(cpu, hazptr_waiter_mask(w, old_idx)) &&
			    !test_bit(cpu, hazptr_waiter_mask(w, new_idx)))
				continue;	/* Both bits already clear. */
			for (idx = 0; idx < NR_HAZPTR_PERCPU_SLOTS; idx++) {
				if (vals[idx] == w->addr) {
					has_addr = true;
					break;
				}
			}
			if (!has_addr)
				has_addr = hazptr_ovf_list_blocked(cpu, w->addr,
					old_wc, new_wc, &has_old, &has_new);
			if (has_addr)
				continue;
			if (!has_old)
				__clear_bit(cpu, hazptr_waiter_mask(w, old_idx));
			if (!has_new)
				__clear_bit(cpu, hazptr_waiter_mask(w, new_idx));
		}
	}

	mutex_unlock(&hazptr_wildcard_lock);

	/* Complete waiters whose masks are both empty. */
	list_for_each_entry_safe(w, n, &hazptr_scan.scanning, node) {
		if (bitmap_empty(hazptr_waiter_mask(w, 0), nr_cpu_ids) &&
		    bitmap_empty(hazptr_waiter_mask(w, 1), nr_cpu_ids))
			list_move(&w->node, &done);
	}

	list_for_each_entry_safe(w, n, &done, node) {
		list_del_init(&w->node);
		complete(&w->done);
	}
}

/*
 * Shared scan kthread for hazptr_synchronize() waiters.
 */
static int hazptr_scan_kthread(void *unused)
{
	for (;;) {
		bool idle;

		swait_event_idle_exclusive(hazptr_scan.wq,
					   READ_ONCE(hazptr_scan.wakeup));

		hazptr_scan_do_cycle();

		mutex_lock(&hazptr_scan.lock);
		idle = list_empty(&hazptr_scan.pending) &&
		      list_empty(&hazptr_scan.scanning);
		if (idle)
			WRITE_ONCE(hazptr_scan.wakeup, false);
		mutex_unlock(&hazptr_scan.lock);

		if (idle)
			continue;
		/* Waiters still blocked: retry after a polling delay. */
		schedule_timeout_idle(1);
	}
	return 0;
}

/*
 * Queue @addr for scan-thread processing, then sleep until the scan
 * thread observes that @addr is no longer held by any hazard pointer.
 * Returns false if the waiter masks cannot be allocated, in which
 * case the caller falls back to the direct scan.
 */
static bool hazptr_synchronize_queued(void *addr)
{
	struct hazptr_waiter waiter = {
		.addr = addr,
	};
	unsigned long *masks;
	unsigned int mask_longs = BITS_TO_LONGS(nr_cpu_ids);

	masks = kcalloc(2, mask_longs * sizeof(unsigned long), GFP_KERNEL);
	if (!masks)
		return false;
	bitmap_fill(masks, nr_cpu_ids);
	bitmap_fill(masks + mask_longs, nr_cpu_ids);
	waiter.cpu_mask = masks;

	init_completion(&waiter.done);
	INIT_LIST_HEAD(&waiter.node);

	/* Enqueue and wake the scan kthread. */
	mutex_lock(&hazptr_scan.lock);
	list_add_tail(&waiter.node, &hazptr_scan.pending);
	if (!READ_ONCE(hazptr_scan.wakeup)) {
		WRITE_ONCE(hazptr_scan.wakeup, true);
		swake_up_one(&hazptr_scan.wq);
	}
	mutex_unlock(&hazptr_scan.lock);

	/* Sleep until the scan thread completes this waiter. */
	wait_for_completion(&waiter.done);
	kfree(masks);
	return true;
}

/*
 * hazptr_synchronize: Wait until @addr is released from all slots.
 *
 * Wait to observe that each slot contains a value that differs from
 * @addr before returning.
 * Should be called from preemptible context.
 *
 * If the scan kthread is running, the caller is queued and the scan
 * thread performs the work, allowing multiple concurrent callers to
 * share a single scan cycle.  Otherwise, the existing direct
 * two-phase scan is used as a fallback.
 */
void hazptr_synchronize(void *addr)
{
	void *scan_wildcard;

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

	/* Use the scan thread if available. */
	/* Pairs with smp_store_release in hazptr_scan_init(). */
	if (smp_load_acquire(&hazptr_scan.kthread) &&
	    hazptr_synchronize_queued(addr))
		return;

	/* Fallback: direct two-phase wildcard scan. */
	guard(mutex)(&hazptr_wildcard_lock);
	scan_wildcard = flip_wildcard(hazptr_wildcard);
	hazptr_scan_period(addr, scan_wildcard);
	WRITE_ONCE(hazptr_wildcard, scan_wildcard);	/* Flip the current wildcard. */
	hazptr_scan_period(addr, flip_wildcard(scan_wildcard));
}
EXPORT_SYMBOL_GPL(hazptr_synchronize);

struct hazptr_slot *hazptr_chain_backup_slot(struct hazptr_ctx *ctx)
{
	struct hazptr_overflow_list_flip *overflow_list_flip = this_cpu_ptr(&percpu_overflow_list_flip);
	unsigned int list_idx = (unsigned long) READ_ONCE(hazptr_wildcard) - 1;
	struct hazptr_overflow_list *overflow_list = &overflow_list_flip->array[list_idx];
	struct hazptr_slot *slot = &ctx->backup_slot.slot;

	slot->addr = NULL;
	guard(raw_spinlock_irqsave)(&overflow_list->lock);
	overflow_list->gen++;
	hlist_add_head(&ctx->backup_slot.overflow_node, &overflow_list->head);
	ctx->backup_slot.overflow_list = overflow_list;
	return slot;
}
EXPORT_SYMBOL_GPL(hazptr_chain_backup_slot);

void hazptr_unchain_backup_slot(struct hazptr_ctx *ctx)
{
	struct hazptr_overflow_list *overflow_list = ctx->backup_slot.overflow_list;

	guard(raw_spinlock_irqsave)(&overflow_list->lock);
	overflow_list->gen++;
	hlist_del(&ctx->backup_slot.overflow_node);
}
EXPORT_SYMBOL_GPL(hazptr_unchain_backup_slot);

void __init hazptr_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct hazptr_overflow_list_flip *overflow_list_flip = per_cpu_ptr(&percpu_overflow_list_flip, cpu);

		for (int i = 0; i < 2; i++) {
			raw_spin_lock_init(&overflow_list_flip->array[i].lock);
			INIT_HLIST_HEAD(&overflow_list_flip->array[i].head);
		}
	}
}

/*
 * Initialize the scan kthread.  On failure falls back to the direct
 * scan (busy-wait) path at synchronize time.
 * core_initcall ensures the scheduler is ready before kthread_run.
 */
static int __init hazptr_scan_init(void)
{
	struct task_struct *t;

	init_swait_queue_head(&hazptr_scan.wq);
	mutex_init(&hazptr_scan.lock);
	INIT_LIST_HEAD(&hazptr_scan.pending);
	INIT_LIST_HEAD(&hazptr_scan.scanning);

	t = kthread_run(hazptr_scan_kthread, NULL, "hazptr_scan");
	if (!IS_ERR(t))
		/* Pairs with smp_load_acquire in hazptr_synchronize(). */
		smp_store_release(&hazptr_scan.kthread, t);
	else
		pr_warn("hazptr: scan thread failed, using direct scan\n");
	return 0;
}
core_initcall(hazptr_scan_init);
