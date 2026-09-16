/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_COUNTER_H
#define _LINUX_PAGE_COUNTER_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/limits.h>
#include <linux/percpu.h>
#include <linux/local_lock.h>
#include <linux/workqueue_types.h>
#include <asm/page.h>

/*
 * The value of NR_PAGE_COUNTER_STOCK is selected to keep the cached counters
 * and their nr_pages in a single cacheline. This may change in the future.
 */
#define NR_PAGE_COUNTER_STOCK 7
#define PAGE_COUNTER_STOCK_BATCH 64UL
struct cgroup_subsys_state;
struct page_counter;
struct page_counter_stock_pcp {
	local_trylock_t lock;
	u8 nr_pages[NR_PAGE_COUNTER_STOCK];
	struct page_counter *cached[NR_PAGE_COUNTER_STOCK];

	struct page_counter_stock_pcp __percpu *base;
	struct work_struct work;
	unsigned long flags;
	u8 drain_idx;
};

struct page_counter {
	/*
	 * Make sure 'usage' does not share cacheline with any other field in
	 * v2. The memcg->memory.usage is a hot member of struct mem_cgroup.
	 */
	atomic_long_t usage;
	unsigned long failcnt; /* v1-only field */

	CACHELINE_PADDING(_pad1_);

	/* effective memory.min and memory.min usage tracking */
	unsigned long emin;
	atomic_long_t min_usage;
	atomic_long_t children_min_usage;

	/* effective memory.low and memory.low usage tracking */
	unsigned long elow;
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	unsigned long watermark;
	/* Latest cg2 reset watermark */
	unsigned long local_watermark;

	/* Keep all the read most fields in a separete cacheline. */
	CACHELINE_PADDING(_pad2_);

	bool protection_support;
	bool track_failcnt;
	unsigned long min;
	unsigned long low;
	unsigned long high;
	unsigned long max;
	struct page_counter *parent;
	struct page_counter_stock_pcp __percpu *stock;
	struct cgroup_subsys_state *stock_css;
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
	counter->usage = (atomic_long_t)ATOMIC_LONG_INIT(0);
	counter->max = PAGE_COUNTER_MAX;
	counter->parent = parent;
	counter->protection_support = protection_support;
	counter->track_failcnt = false;
	counter->stock = NULL;
	counter->stock_css = NULL;
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
void page_counter_refill_stock(struct page_counter *counter,
			       unsigned long nr_pages);
void page_counter_drain_stock_fully(struct page_counter_stock_pcp *stock);
bool page_counter_stock_flush_required(struct page_counter_stock_pcp *stock,
				       struct cgroup_subsys_state *root_css);
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
