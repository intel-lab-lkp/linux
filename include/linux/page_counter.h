/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/limits.h>
#include <asm/page.h>

/*
 * Hierarchical protection (memory.min / memory.low) tracking.
 *
 * Only the memory page counter (and dmem pools) participate in protection.
 * swap/memsw, kmem and tcpmem page counters never do, so the protection
 * fields are kept out of struct page_counter in this separate structure to
 * save space in the common case. struct page_counter links to it via ->prot,
 * which is NULL for counters without protection support.
 */
struct page_counter_protection {
	/*
	 * Fields up to and including children_low_usage are read and
	 * updated by propagate_protected_usage() on every charge and
	 * uncharge, once per level of the hierarchy.  Keep them together
	 * so that they share a cache line: both embedders (struct
	 * mem_cgroup and the dmem pool state) place this structure at a
	 * cache line boundary.
	 */
	struct page_counter_protection *parent;

	unsigned long min;
	unsigned long low;

	/* memory.min and memory.low usage tracking */
	atomic_long_t min_usage;
	atomic_long_t low_usage;
	atomic_long_t children_min_usage;
	atomic_long_t children_low_usage;

	/*
	 * Effective values, recomputed by
	 * page_counter_calculate_protection() during reclaim and read by
	 * the mem_cgroup and dmem protection checks.  Not touched by the
	 * charge path.
	 */
	unsigned long emin;
	unsigned long elow;
};

struct page_counter {
	/*
	 * Make sure 'usage' does not share cacheline with any other field in
	 * v2. The memcg->memory.usage is a hot member of struct mem_cgroup.
	 */
	atomic_long_t usage;
	unsigned long failcnt; /* v1-only field */

	CACHELINE_PADDING(_pad1_);

	unsigned long watermark;
	/* Latest cg2 reset watermark */
	unsigned long local_watermark;

	bool track_failcnt;
	unsigned long high;
	unsigned long max;
	struct page_counter *parent;

	/*
	 * Hierarchical protection context, NULL for counters that do not
	 * support memory.min/memory.low (swap, memsw, kmem, tcpmem, ...).
	 */
	struct page_counter_protection *prot;
} ____cacheline_internodealigned_in_smp;

#if BITS_PER_LONG == 32
#define PAGE_COUNTER_MAX LONG_MAX
#else
#define PAGE_COUNTER_MAX (LONG_MAX / PAGE_SIZE)
#endif

static inline void page_counter_init(struct page_counter *counter,
				     struct page_counter *parent)
{
	counter->usage = (atomic_long_t)ATOMIC_LONG_INIT(0);
	counter->max = PAGE_COUNTER_MAX;
	counter->parent = parent;
	counter->track_failcnt = false;
	counter->prot = NULL;
}

/*
 * Enable hierarchical protection (memory.min/memory.low) on @counter.
 * @prot and @parent are the protection contexts of @counter and its
 * parent page counter respectively. Only the memory page counter (and
 * dmem pools) call this.
 *
 * The remaining members of @prot (emin, elow and the usage counters) are
 * expected to be zero already, so @prot must come from zeroed memory.
 */
static inline void page_counter_init_protection(struct page_counter *counter,
						struct page_counter_protection *prot,
						struct page_counter_protection *parent)
{
	counter->prot = prot;
	prot->parent = parent;
	prot->min = 0;
	prot->low = 0;
}

static inline unsigned long page_counter_read(struct page_counter *counter)
{
	return atomic_long_read(&counter->usage);
}

long page_counter_margin(struct page_counter *counter);
void page_counter_cancel(struct page_counter *counter, unsigned long nr_pages);
void page_counter_charge(struct page_counter *counter, unsigned long nr_pages);
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long nr_pages,
			     struct page_counter **fail);
void page_counter_uncharge(struct page_counter *counter, unsigned long nr_pages);
void page_counter_set_min(struct page_counter *counter, unsigned long nr_pages);
void page_counter_set_low(struct page_counter *counter, unsigned long nr_pages);

static inline void page_counter_set_high(struct page_counter *counter,
					 unsigned long nr_pages)
{
	WRITE_ONCE(counter->high, nr_pages);
}

int page_counter_set_max(struct page_counter *counter, unsigned long nr_pages);
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages);

static inline void page_counter_reset_watermark(struct page_counter *counter)
{
	unsigned long usage = page_counter_read(counter);

	/*
	 * Update local_watermark first, so it's always <= watermark
	 * (modulo CPU/compiler re-ordering)
	 */
	counter->local_watermark = usage;
	counter->watermark = usage;
}

#if IS_ENABLED(CONFIG_MEMCG) || IS_ENABLED(CONFIG_CGROUP_DMEM)
void page_counter_calculate_protection(struct page_counter *root,
				       struct page_counter *counter,
				       bool recursive_protection);
#else
static inline void page_counter_calculate_protection(struct page_counter *root,
						     struct page_counter *counter,
						     bool recursive_protection) {}
#endif

#endif /* _LINUX_PAGE_COUNTER_H */
