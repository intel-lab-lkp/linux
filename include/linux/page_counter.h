/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/limits.h>
#include <asm/page.h>

/*
 * Page counters are used by memory and hugetlb cgroups.
 * Memory cgroups are using up to 4 separate counters:
 * memory, swap (memory + swap on cgroup v1), kmem and tcpmem.
 * Hugetlb cgroups are using 2 * HUGE_MAX_HSTATE separate
 * counters: for tracking the usage and reservations of each
 * supported hugepage size.
 */

#ifdef CONFIG_CGROUP_HUGETLB
#ifdef HUGE_MAX_HSTATE
#define __MCT_HUGETLB_MAX (HUGE_MAX_HSTATE * 2 - 1)
#else
#define __MCT_HUGETLB_MAX 1
#endif
#endif /* CONFIG_CGROUP_HUGETLB */

enum mem_counter_type {
#ifdef CONFIG_MEMCG
	MCT_MEMORY,		/* cgroup v1 and v2 */
	MCT_SWAP,		/* cgroup v2 only */
	MCT_MEMSW = MCT_SWAP,	/* cgroup v1 only */
	MCT_KMEM,		/* cgroup v1 only */
	MCT_TCPMEM,		/* cgroup v1 only */
#endif
#ifdef CONFIG_CGROUP_HUGETLB
	MCT_HUGETLB_MAX = __MCT_HUGETLB_MAX,
#endif
	__MCT_NR_ITEMS,
};

struct page_counter {
	/*
	 * Make sure 'usage' does not share cacheline with any other field. The
	 * memcg->memory.usage is a hot member of struct mem_cgroup.
	 */
	atomic_long_t usage[__MCT_NR_ITEMS];
	CACHELINE_PADDING(_pad1_);

	/* effective memory.min and memory.min usage tracking */
	unsigned long emin;
	atomic_long_t min_usage;
	atomic_long_t children_min_usage;

	/* effective memory.low and memory.low usage tracking */
	unsigned long elow;
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	unsigned long watermark[__MCT_NR_ITEMS];
	unsigned long failcnt[__MCT_NR_ITEMS];

	/* Keep all the read most fields in a separete cacheline. */
	CACHELINE_PADDING(_pad2_);

	bool protection_support;
	unsigned long min;
	unsigned long low;
	unsigned long high[__MCT_NR_ITEMS];
	unsigned long max[__MCT_NR_ITEMS];
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
	int i;

	for (i = 0; i < __MCT_NR_ITEMS; i++) {
		counter->usage[i] = (atomic_long_t)ATOMIC_LONG_INIT(0);
		counter->max[i] = PAGE_COUNTER_MAX;
	}

	counter->parent = parent;
	counter->protection_support = protection_support;
}

static inline unsigned long page_counter_read(struct page_counter *counter,
	enum mem_counter_type id)
{
	return atomic_long_read(&counter->usage[id]);
}

void page_counter_cancel(struct page_counter *counter,
			 enum mem_counter_type id,
			 unsigned long nr_pages);
void page_counter_charge(struct page_counter *counter,
			 enum mem_counter_type id,
			 unsigned long nr_pages);
bool page_counter_try_charge(struct page_counter *counter,
			     enum mem_counter_type id,
			     unsigned long nr_pages,
			     struct page_counter **fail);
void page_counter_uncharge(struct page_counter *counter,
			   enum mem_counter_type id,
			   unsigned long nr_pages);
void page_counter_set_min(struct page_counter *counter, unsigned long nr_pages);
void page_counter_set_low(struct page_counter *counter, unsigned long nr_pages);

static inline void page_counter_set_high(struct page_counter *counter,
					 enum mem_counter_type id,
					 unsigned long nr_pages)
{
	WRITE_ONCE(counter->high[id], nr_pages);
}

int page_counter_set_max(struct page_counter *counter,
			 enum mem_counter_type id,
			 unsigned long nr_pages);
int page_counter_memparse(const char *buf, const char *max,
			  unsigned long *nr_pages);

static inline void page_counter_reset_watermark(struct page_counter *counter,
						enum mem_counter_type id)
{
	counter->watermark[id] = page_counter_read(counter, id);
}

#endif /* _LINUX_PAGE_COUNTER_H */
