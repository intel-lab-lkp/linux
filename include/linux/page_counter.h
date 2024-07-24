/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/limits.h>
#include <linux/mm_types.h>
#include <asm/page.h>

/*
 * Page counters are used by memory and hugetlb cgroups.
 * Memory cgroups are using up to 4 separate counters:
 * memory, swap (memory + swap on cgroup v1), kmem and tcpmem.
 * Hugetlb cgroups are using 2 arrays of counters with HUGE_MAX_HSTATE
 * in each to track the usage and reservations of each supported
 * hugepage size.
 *
 * Protection (min/low) is supported only for the first counter
 * with idx 0 and only if the counter was initialized with the protection
 * support.
 */

enum mem_counter_type {
#ifdef CONFIG_MEMCG
	/* Unified memory counter */
	MCT_MEM,

	/* Swap */
	MCT_SWAP,

	/* Memory + swap */
	MCT_MEMSW = MCT_SWAP,

#ifdef CONFIG_MEMCG_V1
	/* Kernel memory */
	MCT_KMEM,

	/* Tcp memory */
	MCT_TCPMEM,
#endif /* CONFIG_MEMCG_V1 */
#endif /* CONFIG_MEMCG */

	/* Maximum number of memcg counters */
	MCT_NR_MEMCG_ITEMS,
};

#ifdef CONFIG_CGROUP_HUGETLB
#ifdef HUGE_MAX_HSTATE
#define MCT_NR_HUGETLB_ITEMS HUGE_MAX_HSTATE
#else
#define MCT_NR_HUGETLB_ITEMS 1
#endif

/*
 * max() can't be used here: even though __builtin_choose_expr() evaluates
 * to true, the false clause generates a compiler error:
 * error: braced-group within expression allowed only inside a function .
 */
#define MCT_NR_ITEMS (__builtin_choose_expr(MCT_NR_MEMCG_ITEMS > MCT_NR_HUGETLB_ITEMS, \
					    MCT_NR_MEMCG_ITEMS, MCT_NR_HUGETLB_ITEMS))

#else /* CONFIG_CGROUP_HUGETLB */
#define MCT_NR_ITEMS MCT_NR_MEMCG_ITEMS
#endif /* CONFIG_CGROUP_HUGETLB */

struct page_counter {
	/*
	 * Make sure 'usage' does not share cacheline with any other field. The
	 * memcg->memory.usage is a hot member of struct mem_cgroup.
	 */
	atomic_long_t usage[MCT_NR_ITEMS];
	CACHELINE_PADDING(_pad1_);

	/* effective memory.min and memory.min usage tracking */
	unsigned long emin;
	atomic_long_t min_usage;
	atomic_long_t children_min_usage;

	/* effective memory.low and memory.low usage tracking */
	unsigned long elow;
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	unsigned long watermark[MCT_NR_ITEMS];
	unsigned long local_watermark[MCT_NR_ITEMS]; /* track min of fd-local resets */
	unsigned long failcnt[MCT_NR_ITEMS];

	/* Keep all the read most fields in a separete cacheline. */
	CACHELINE_PADDING(_pad2_);

	bool protection_support;
	unsigned long min;
	unsigned long low;
	unsigned long high[MCT_NR_ITEMS];
	unsigned long max[MCT_NR_ITEMS];

	struct page_counter *parent;
} ____cacheline_internodealigned_in_smp;

#if BITS_PER_LONG == 32
#define PAGE_COUNTER_MAX LONG_MAX
#else
#define PAGE_COUNTER_MAX (LONG_MAX / PAGE_SIZE)
#endif

/*
 * Protection is supported only for the first counter (with id 0).
 */
static inline void page_counter_init(struct page_counter *counter,
				     struct page_counter *parent,
				     bool protection_support)
{
	unsigned long i;

	for (i = 0; i < MCT_NR_ITEMS; i++) {
		counter->usage[i] = (atomic_long_t)ATOMIC_LONG_INIT(0);
		counter->max[i] = PAGE_COUNTER_MAX;
	}
	counter->parent = parent;
	counter->protection_support = protection_support;
}

static inline unsigned long page_counter_read(struct page_counter *counter,
					      unsigned long idx)
{
	return atomic_long_read(&counter->usage[idx]);
}

void page_counter_cancel(struct page_counter *counter, unsigned long idx,
			 unsigned long nr_pages);
void page_counter_charge(struct page_counter *counter, unsigned long idx,
			 unsigned long nr_pages);
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long idx,
			     unsigned long nr_pages,
			     struct page_counter **fail);
void page_counter_uncharge(struct page_counter *counter, unsigned long idx,
			   unsigned long nr_pages);
void page_counter_set_min(struct page_counter *counter, unsigned long idx,
			  unsigned long nr_pages);
void page_counter_set_low(struct page_counter *counter, unsigned long idx,
			  unsigned long nr_pages);

static inline void page_counter_set_high(struct page_counter *counter,
					 unsigned long idx,
					 unsigned long nr_pages)
{
	WRITE_ONCE(counter->high[idx], nr_pages);
}

int page_counter_set_max(struct page_counter *counter, unsigned long idx,
			 unsigned long nr_pages);
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages);

static inline void page_counter_reset_watermark(struct page_counter *counter,
						unsigned long idx)
{
	unsigned long usage = page_counter_read(counter, idx);

	counter->watermark[idx] = usage;
	counter->local_watermark[idx] = usage;
}

#ifdef CONFIG_MEMCG
void page_counter_calculate_protection(struct page_counter *root,
				       struct page_counter *counter,
				       bool recursive_protection);
#else
static inline void page_counter_calculate_protection(struct page_counter *root,
						     struct page_counter *counter,
						     bool recursive_protection) {}
#endif

#endif /* _LINUX_PAGE_COUNTER_H */
