/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simple hazard pointers
 *
 * Copyright (c) 2025, Microsoft Corporation.
 *
 * Author: Boqun Feng <boqun.feng@gmail.com>
 */

#define pr_fmt(fmt) "shazptr: " fmt

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/shazptr.h>
#include <linux/slab.h>
#include <linux/sort.h>

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "shazptr."

DEFINE_PER_CPU_SHARED_ALIGNED(void *, shazptr_slots);
EXPORT_PER_CPU_SYMBOL_GPL(shazptr_slots);

/* Wait structure for synchronize_shazptr(). */
struct shazptr_wait {
	struct list_head list;
	/* Which groups of CPUs are blocking. */
	unsigned long blocking_grp_mask;
	void *ptr;
	struct completion done;
};

/* Snapshot for hazptr slot. */
struct shazptr_snapshot {
	unsigned long ptr;
	unsigned long grp_mask;
};

static inline int
shazptr_snapshot_cmp(const void *a, const void *b)
{
	const struct shazptr_snapshot *snap_a = (struct shazptr_snapshot *)a;
	const struct shazptr_snapshot *snap_b = (struct shazptr_snapshot *)b;

	if (snap_a->ptr > snap_b->ptr)
		return 1;
	else if (snap_a->ptr < snap_b->ptr)
		return -1;
	else
		return 0;
}

/* *In-place* merge @n together based on ->ptr and accumulate the >grp_mask. */
static int shazptr_snapshot_merge(struct shazptr_snapshot *snaps, int n)
{
	int new, i;

	/* Sort first. */
	sort(snaps, n, sizeof(*snaps), shazptr_snapshot_cmp, NULL);

	new = 0;

	/* Skip NULLs. */
	for (i = 0; i < n; i++) {
		if (snaps[i].ptr)
			break;
	}

	while (i < n) {
		/* Start with a new address. */
		snaps[new] = snaps[i];

		for (; i < n; i++) {
			/* Merge if the next one has the same address. */
			if (snaps[new].ptr == snaps[i].ptr) {
				snaps[new].grp_mask |= snaps[i].grp_mask;
			} else
				break;
		}

		/*
		 * Either the end has been reached or need to start with a new
		 * record.
		 */
		new++;
	}

	return new;
}

/*
 * Calculate which group is still blocking @ptr, this assumes the @snaps is
 * already merged.
 */
static unsigned long
shazptr_snapshot_blocking_grp_mask(struct shazptr_snapshot *snaps,
				   int n, void *ptr)
{
	unsigned long mask = 0;

	if (!n)
		return mask;
	else if (snaps[n-1].ptr == (unsigned long)SHAZPTR_WILDCARD) {
		/*
		 * Take SHAZPTR_WILDCARD slots, which is ULONG_MAX, into
		 * consideration if any.
		 */
		mask = snaps[n-1].grp_mask;
	}

	/* TODO: binary search if n is big. */
	for (int i = 0; i < n; i++) {
		if (snaps[i].ptr == (unsigned long)ptr) {
			mask |= snaps[i].grp_mask;
			break;
		}
	}

	return mask;
}

/* Scan structure for synchronize_shazptr(). */
struct shazptr_scan {
	/* The scan kthread */
	struct task_struct *thread;

	/* Wait queue for the scan kthread */
	struct swait_queue_head wq;

	/* Whether the scan kthread has been scheduled to scan */
	bool scheduled;

	/* The lock protecting ->queued and ->scheduled */
	struct mutex lock;

	/* List of queued synchronize_shazptr() request. */
	struct list_head queued;

	int cpu_grp_size;

	/* List of scanning synchronize_shazptr() request. */
	struct list_head scanning;

	/* Buffer used for hazptr slot scan, nr_cpu_ids slots*/
	struct shazptr_snapshot* snaps;
};

static struct shazptr_scan shazptr_scan;

static void shazptr_do_scan(struct shazptr_scan *scan)
{
	int cpu;
	int snaps_len;
	struct shazptr_wait *curr, *next;

	scoped_guard(mutex, &scan->lock) {
		/* Move from ->queued to ->scanning. */
		list_splice_tail_init(&scan->queued, &scan->scanning);
	}

	memset(scan->snaps, nr_cpu_ids, sizeof(struct shazptr_snapshot));

	for_each_possible_cpu(cpu) {
		void **slot = per_cpu_ptr(&shazptr_slots, cpu);
		void *val;

		/* Pair with smp_store_release() in shazptr_clear(). */
		val = smp_load_acquire(slot);

		scan->snaps[cpu].ptr = (unsigned long)val;
		scan->snaps[cpu].grp_mask = 1UL << (cpu / scan->cpu_grp_size);
	}

	snaps_len = shazptr_snapshot_merge(scan->snaps, nr_cpu_ids);

	/* Only one thread can access ->scanning, so can be lockless. */
	list_for_each_entry_safe(curr, next, &scan->scanning, list) {
		/* Accumulate the shazptr slot scan result. */
		curr->blocking_grp_mask &=
			shazptr_snapshot_blocking_grp_mask(scan->snaps,
							   snaps_len,
							   curr->ptr);

		if (curr->blocking_grp_mask == 0) {
			/* All shots are observed as not blocking once. */
			list_del(&curr->list);
			complete(&curr->done);
		}
	}
}

static int __noreturn shazptr_scan_kthread(void *unused)
{
	for (;;) {
		swait_event_idle_exclusive(shazptr_scan.wq,
					   READ_ONCE(shazptr_scan.scheduled));

		shazptr_do_scan(&shazptr_scan);

		scoped_guard(mutex, &shazptr_scan.lock) {
			if (list_empty(&shazptr_scan.queued) &&
			    list_empty(&shazptr_scan.scanning))
				shazptr_scan.scheduled = false;
		}
	}
}

static int __init shazptr_scan_init(void)
{
	struct shazptr_scan *scan = &shazptr_scan;
	struct task_struct *t;

	init_swait_queue_head(&scan->wq);
	mutex_init(&scan->lock);
	INIT_LIST_HEAD(&scan->queued);
	INIT_LIST_HEAD(&scan->scanning);
	scan->scheduled = false;

	/* Group CPUs into at most BITS_PER_LONG groups. */
	scan->cpu_grp_size = DIV_ROUND_UP(nr_cpu_ids, BITS_PER_LONG);

	scan->snaps = kcalloc(nr_cpu_ids, sizeof(scan->snaps[0]), GFP_KERNEL);

	if (scan->snaps) {
		t = kthread_run(shazptr_scan_kthread, NULL, "shazptr_scan");
		if (!IS_ERR(t)) {
			smp_store_release(&scan->thread, t);
			/* Kthread creation succeeds */
			return 0;
		} else {
			kfree(scan->snaps);
		}
	}

	pr_info("Failed to create the scan thread, only busy waits\n");
	return 0;
}
core_initcall(shazptr_scan_init);

static void synchronize_shazptr_busywait(void *ptr)
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

/* Disabled by default. */
static int skip_synchronize_self_scan;
module_param(skip_synchronize_self_scan, int, 0644);

static void synchronize_shazptr_normal(void *ptr)
{
	int cpu;
	unsigned long blocking_grp_mask = 0;

	smp_mb(); /* Synchronize with the smp_mb() in shazptr_acquire(). */

	if (unlikely(skip_synchronize_self_scan)) {
		blocking_grp_mask = ~0UL;
	} else {
		for_each_possible_cpu(cpu) {
			void **slot = per_cpu_ptr(&shazptr_slots, cpu);
			void *val;

			/* Pair with smp_store_release() in shazptr_clear(). */
			val = smp_load_acquire(slot);

			if (val == ptr || val == SHAZPTR_WILDCARD)
				blocking_grp_mask |= 1UL << (cpu / shazptr_scan.cpu_grp_size);
		}
	}

	/* Found blocking slots, prepare to wait. */
	if (blocking_grp_mask) {
		struct shazptr_scan *scan = &shazptr_scan;
		struct shazptr_wait wait = {
			.blocking_grp_mask = blocking_grp_mask,
		};

		INIT_LIST_HEAD(&wait.list);
		init_completion(&wait.done);

		scoped_guard(mutex, &scan->lock) {
			list_add_tail(&wait.list, &scan->queued);

			if (!scan->scheduled) {
				WRITE_ONCE(scan->scheduled, true);
				swake_up_one(&shazptr_scan.wq);
			}
		}

		wait_for_completion(&wait.done);
	}
}

void synchronize_shazptr(void *ptr)
{
	/* Busy waiting if the scan kthread has not been created. */
	if (!smp_load_acquire(&shazptr_scan.thread))
		synchronize_shazptr_busywait(ptr);
	else
		synchronize_shazptr_normal(ptr);
}
EXPORT_SYMBOL_GPL(synchronize_shazptr);
